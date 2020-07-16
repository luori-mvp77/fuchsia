// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package netstack

import (
	"go.fuchsia.dev/fuchsia/src/connectivity/network/netstack/link"

	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/buffer"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
)

type noopEndpoint struct {
	linkAddress tcpip.LinkAddress
	attached    chan struct{}
}

func (*noopEndpoint) MTU() uint32 {
	return 0
}

func (*noopEndpoint) Capabilities() stack.LinkEndpointCapabilities {
	return 0
}

func (*noopEndpoint) MaxHeaderLength() uint16 {
	return 0
}

func (ep *noopEndpoint) LinkAddress() tcpip.LinkAddress {
	return ep.linkAddress
}

func (*noopEndpoint) WritePacket(*stack.Route, *stack.GSO, tcpip.NetworkProtocolNumber, *stack.PacketBuffer) *tcpip.Error {
	return nil
}

func (*noopEndpoint) WritePackets(*stack.Route, *stack.GSO, stack.PacketBufferList, tcpip.NetworkProtocolNumber) (int, *tcpip.Error) {
	return 0, nil
}

func (*noopEndpoint) WriteRawPacket(buffer.VectorisedView) *tcpip.Error {
	return nil
}

func (ep *noopEndpoint) Attach(dispatcher stack.NetworkDispatcher) {
	if dispatcher != nil {
		ep.attached = make(chan struct{})
	} else {
		close(ep.attached)
		ep.attached = nil
	}
}

func (ep *noopEndpoint) IsAttached() bool {
	return ep.attached != nil
}

func (ep *noopEndpoint) Wait() {
	if ch := ep.attached; ch != nil {
		<-ch
	}
}

type noopController struct {
	onUp                 func()
	onStateChange        func(link.State)
	onSetPromiscuousMode func(bool)
}

func (n *noopController) Up() error {
	if fn := n.onUp; fn != nil {
		fn()
	}
	n.onStateChange(link.StateStarted)
	return nil
}

func (n *noopController) Down() error {
	n.onStateChange(link.StateDown)
	return nil
}

func (n *noopController) Close() error {
	n.onStateChange(link.StateClosed)
	return nil
}

func (n *noopController) SetOnStateChange(fn func(link.State)) {
	n.onStateChange = fn
}

func (ep *noopController) SetPromiscuousMode(v bool) error {
	if fn := ep.onSetPromiscuousMode; fn != nil {
		fn(v)
	}
	return nil
}

func addNoopEndpoint(ns *Netstack, name string) (*ifState, error) {
	return ns.addEndpoint(makeEndpointName("test", name), &noopEndpoint{}, &noopController{}, true, 0, false)
}