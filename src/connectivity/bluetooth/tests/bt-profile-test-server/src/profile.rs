// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::format_err,
    fidl_fuchsia_bluetooth_bredr::{
        DataElement, ProtocolDescriptor, ProtocolIdentifier, ServiceClassProfileIdentifier,
        ServiceDefinition,
    },
    fuchsia_bluetooth::types::Uuid,
    std::collections::HashSet,
    std::convert::TryFrom,
};

use crate::types::{Psm, ServiceRecord};

/// Builds the L2Cap Protocol Descriptor from the provided `psm`.
pub fn build_l2cap_descriptor(psm: Psm) -> Vec<ProtocolDescriptor> {
    vec![ProtocolDescriptor {
        protocol: ProtocolIdentifier::L2Cap,
        params: vec![DataElement::Uint16(psm.0)],
    }]
}

/// Attempts to parse the PSM from the protocol descriptor list.
/// Returns an Error if the ProtocolDescriptorList is formatted incorrectly.
fn psm_from_protocol_descriptor_list(
    prot_desc_list: &[ProtocolDescriptor],
) -> Result<Psm, anyhow::Error> {
    for prot_desc in prot_desc_list {
        if prot_desc.protocol == ProtocolIdentifier::L2Cap {
            if prot_desc.params.is_empty() {
                return Err(format_err!("No PSM provided in primary protocol desc list."));
            }

            // The PSM is always the first element.
            if let DataElement::Uint16(psm) = prot_desc.params[0] {
                return Ok(Psm(psm));
            } else {
                return Err(format_err!("Protocol descriptor has invalid format."));
            }
        }
    }

    Err(format_err!("No L2Cap entry in ProtocolDescriptorList."))
}

fn parse_service_definition(def: &ServiceDefinition) -> Result<ServiceRecord, anyhow::Error> {
    // Parse the service class UUIDs into ServiceClassProfileIdentifiers.
    let svc_ids = match &def.service_class_uuids {
        Some(uuids) if !uuids.is_empty() => {
            let mut svc_ids = HashSet::new();
            for id in uuids {
                let uuid: Uuid = id.into();
                let svc_id = ServiceClassProfileIdentifier::try_from(uuid)?;
                svc_ids.insert(svc_id);
            }
            svc_ids
        }
        _ => return Err(format_err!("Service definition must contain at least one service class")),
    };

    // Parse the primary protocol descriptor list. This field is optional. If it exists,
    // attempt to parse out the PSM.
    let primary_psm = if let Some(prot_desc_list) = &def.protocol_descriptor_list {
        Some(psm_from_protocol_descriptor_list(prot_desc_list)?)
    } else {
        None
    };

    // Parse the (optional) additional protocol descriptor lists.
    let mut addl_psms = HashSet::new();
    if let Some(additional_prot_desc_list) = &def.additional_protocol_descriptor_lists {
        for addl_list in additional_prot_desc_list {
            let addl_psm = psm_from_protocol_descriptor_list(addl_list)?;
            addl_psms.insert(addl_psm);
        }
    }

    // Parse the (optional) profile descriptors.
    let mut prof_descriptors = vec![];
    if let Some(profile_descriptors) = &def.profile_descriptors {
        for descriptor in profile_descriptors {
            prof_descriptors.push(descriptor.clone());
        }
    }

    Ok(ServiceRecord::new(svc_ids, primary_psm, addl_psms, prof_descriptors))
}

pub fn parse_service_definitions(
    svc_defs: Vec<ServiceDefinition>,
) -> Result<Vec<ServiceRecord>, anyhow::Error> {
    let mut parsed_svc_defs = vec![];

    for def in &svc_defs {
        let parsed_def = parse_service_definition(def)?;
        parsed_svc_defs.push(parsed_def);
    }

    Ok(parsed_svc_defs)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    use fidl::encoding::Decodable;
    use fidl_fuchsia_bluetooth_bredr::{
        Attribute, ProfileDescriptor, PSM_AVCTP, PSM_AVCTP_BROWSE, PSM_AVDTP,
    };

    const SDP_SUPPORTED_FEATURES: u16 = 0x0311;

    /// Builds the smallest service definition that is valid.
    /// Just includes the ServiceClassProfileIdentifiers.
    /// Returns the expected ServiceRecord as well.
    fn build_minimal_service_definition() -> (ServiceDefinition, ServiceRecord) {
        let def = ServiceDefinition {
            service_class_uuids: Some(vec![
                Uuid::new16(ServiceClassProfileIdentifier::Headset as u16).into(),
                Uuid::new16(ServiceClassProfileIdentifier::Handsfree as u16).into(),
            ]),
            ..ServiceDefinition::new_empty()
        };

        let mut ids = HashSet::new();
        ids.insert(ServiceClassProfileIdentifier::Headset);
        ids.insert(ServiceClassProfileIdentifier::Handsfree);
        let record = ServiceRecord::new(ids, None, HashSet::new(), vec![]);

        (def, record)
    }

    // Builds an A2DP Service Definition and also the expected parsed ServiceRecord.
    // This is done in the same function to maintain data consistency, such that if the
    // parsing implementation changes, only this builder will need to be updated.
    pub(crate) fn build_a2dp_service_definition() -> (ServiceDefinition, ServiceRecord) {
        let prof_descs = vec![ProfileDescriptor {
            profile_id: ServiceClassProfileIdentifier::AdvancedAudioDistribution,
            major_version: 1,
            minor_version: 2,
        }];
        let def = ServiceDefinition {
            service_class_uuids: Some(vec![Uuid::new16(0x110B).into()]), // Audio Sink UUID
            protocol_descriptor_list: Some(vec![
                ProtocolDescriptor {
                    protocol: ProtocolIdentifier::L2Cap,
                    params: vec![DataElement::Uint16(PSM_AVDTP)],
                },
                ProtocolDescriptor {
                    protocol: ProtocolIdentifier::Avdtp,
                    params: vec![DataElement::Uint16(0x0103)], // Indicate v1.3
                },
            ]),
            profile_descriptors: Some(prof_descs.clone()),
            ..ServiceDefinition::new_empty()
        };

        let mut a2dp_ids = HashSet::new();
        a2dp_ids.insert(ServiceClassProfileIdentifier::AudioSink);
        let record = ServiceRecord::new(a2dp_ids, Some(Psm(25)), HashSet::new(), prof_descs);

        (def, record)
    }

    /// Builds an example AVRCP Service Definition and also the expected parsed ServiceRecord.
    /// This is done in the same function to maintain data consistency, such that if the
    /// parsing implementation changes, only this builder will need to be updated.
    fn build_avrcp_service_definition() -> (ServiceDefinition, ServiceRecord) {
        use ServiceClassProfileIdentifier::AvRemoteControl;
        use ServiceClassProfileIdentifier::AvRemoteControlController;
        let prof_descs = vec![ProfileDescriptor {
            profile_id: ServiceClassProfileIdentifier::AvRemoteControl,
            major_version: 1,
            minor_version: 6,
        }];
        let def = ServiceDefinition {
            service_class_uuids: Some(vec![
                Uuid::new16(AvRemoteControl as u16).into(),
                Uuid::new16(AvRemoteControlController as u16).into(),
            ]),
            protocol_descriptor_list: Some(vec![
                ProtocolDescriptor {
                    protocol: ProtocolIdentifier::L2Cap,
                    params: vec![DataElement::Uint16(PSM_AVCTP as u16)],
                },
                ProtocolDescriptor {
                    protocol: ProtocolIdentifier::Avctp,
                    params: vec![DataElement::Uint16(0x0103)], // Indicate v1.3
                },
            ]),
            additional_protocol_descriptor_lists: Some(vec![vec![
                ProtocolDescriptor {
                    protocol: ProtocolIdentifier::L2Cap,
                    params: vec![DataElement::Uint16(PSM_AVCTP_BROWSE as u16)],
                },
                ProtocolDescriptor {
                    protocol: ProtocolIdentifier::Avctp,
                    params: vec![DataElement::Uint16(0x0103)],
                },
            ]]),
            profile_descriptors: Some(prof_descs.clone()),
            additional_attributes: Some(vec![Attribute {
                id: SDP_SUPPORTED_FEATURES, // SDP Attribute "SUPPORTED FEATURES"
                element: DataElement::Uint16(1),
            }]),
            ..ServiceDefinition::new_empty()
        };

        let mut avrcp_ids = HashSet::new();
        avrcp_ids.insert(ServiceClassProfileIdentifier::AvRemoteControl);
        avrcp_ids.insert(ServiceClassProfileIdentifier::AvRemoteControlController);
        let mut additional_psms = HashSet::new();
        additional_psms.insert(Psm(27));
        let record = ServiceRecord::new(avrcp_ids, Some(Psm(23)), additional_psms, prof_descs);

        (def, record)
    }

    /// Builds a list of invalid ServiceDefinitions.
    /// 1. Contains a primary protocol descriptor list, but is empty.
    /// 2. Contains a primary protocol descriptor list, but the PSM is missing.
    /// 3. Contains a primary protocol descriptor list, but the provided PSM is not the first entry.
    fn build_invalid_service_definitions(
    ) -> Vec<(ServiceDefinition, Result<Vec<ServiceRecord>, String>)> {
        use ServiceClassProfileIdentifier::{AudioSink, AvRemoteControl, Hdp};

        let def1 = ServiceDefinition {
            service_class_uuids: Some(vec![Uuid::new16(AudioSink as u16).into()]),
            protocol_descriptor_list: Some(vec![]),
            ..ServiceDefinition::new_empty()
        };
        let err1 = Err(format!("No L2Cap entry in ProtocolDescriptorList."));

        let def2 = ServiceDefinition {
            service_class_uuids: Some(vec![Uuid::new16(AvRemoteControl as u16).into()]),
            protocol_descriptor_list: Some(vec![ProtocolDescriptor {
                protocol: ProtocolIdentifier::L2Cap,
                params: vec![],
            }]),
            ..ServiceDefinition::new_empty()
        };
        let err2 = Err(format!("No PSM provided in primary protocol desc list."));

        let def3 = ServiceDefinition {
            service_class_uuids: Some(vec![Uuid::new16(Hdp as u16).into()]),
            protocol_descriptor_list: Some(vec![ProtocolDescriptor {
                protocol: ProtocolIdentifier::L2Cap,
                params: vec![DataElement::B(true), DataElement::Uint16(PSM_AVCTP_BROWSE as u16)],
            }]),
            ..ServiceDefinition::new_empty()
        };
        let err3 = Err(format!("Protocol descriptor has invalid format."));

        vec![(def1, err1), (def2, err2), (def3, err3)]
    }

    #[test]
    fn test_parse_service_definitions_success() {
        // Empty is ok.
        let empty = vec![];
        let parsed = parse_service_definitions(empty);
        assert_eq!(Ok(vec![]), parsed.map_err(|e| format!("{:?}", e)));

        // Bare minimum case. Only the ServiceClassProfileIdentifiers are provided in the service.
        let (id_only_def, expected_id_only_record) = build_minimal_service_definition();
        let parsed = parse_service_definitions(vec![id_only_def]);
        assert_eq!(Ok(vec![expected_id_only_record]), parsed.map_err(|e| format!("{:?}", e)));

        // Normal case, multiple services.
        let (a2dp_def, expected_a2dp_record) = build_a2dp_service_definition();
        let (avrcp_def, expected_avrcp_record) = build_avrcp_service_definition();
        let service_defs = vec![a2dp_def, avrcp_def];
        let parsed = parse_service_definitions(service_defs);
        assert_eq!(
            Ok(vec![expected_a2dp_record, expected_avrcp_record]),
            parsed.map_err(|e| format!("{:?}", e))
        );
    }

    #[test]
    fn test_parse_invalid_service_definitions() {
        // No service classes is invalid.
        let no_service_id = vec![ServiceDefinition::new_empty()];
        assert_eq!(
            Err(format!("Service definition must contain at least one service class")),
            parse_service_definitions(no_service_id).map_err(|e| format!("{:?}", e))
        );

        // A combination of a valid and invalid services should still result in an error.
        let (id_only_def, _) = build_minimal_service_definition();
        let overall_invalid = vec![id_only_def, ServiceDefinition::new_empty()];
        assert_eq!(
            Err(format!("Service definition must contain at least one service class")),
            parse_service_definitions(overall_invalid).map_err(|e| format!("{:?}", e))
        );

        // Service definitions in which the Primary ProtocolDescriptorList is formatted incorrectly.
        let invalid_defs_with_errs = build_invalid_service_definitions();
        for (invalid_def, expected_err) in invalid_defs_with_errs {
            assert_eq!(
                expected_err,
                parse_service_definitions(vec![invalid_def]).map_err(|e| format!("{:?}", e))
            );
        }
    }
}
