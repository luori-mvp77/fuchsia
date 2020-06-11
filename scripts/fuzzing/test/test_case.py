#!/usr/bin/env python2.7
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import unittest

import test_env
import lib.args
from factory_fake import FakeFactory
from process_fake import FakeProcess


class TestCase(unittest.TestCase):
    """TestCase that provides common test context, utilities, and assertions."""

    # Unit test "constructor"

    def setUp(self):
        super(TestCase, self).setUp()
        self._factory = None
        self._parser = None
        self._host = None
        self._device = None
        self._fuzzer = None
        self._next_pid = 10000

    # Unit test context, lazily instantiated.

    @property
    def factory(self):
        """The associated FakeFactory object."""
        if not self._factory:
            self._factory = FakeFactory()
        return self._factory

    @property
    def cli(self):
        """The associated FakeCLI object."""
        return self.factory.cli

    @property
    def parser(self):
        """The associated ArgParser object."""
        if not self._parser:
            self._parser = self.factory.create_parser()
        return self._parser

    @property
    def host(self):
        """The associated Host object."""
        if not self._host:
            self._host = self.factory.create_host()
        return self._host

    @property
    def device(self):
        """The associated Device object."""
        if not self._device:
            self._device = self.factory.create_device()
        return self._device

    @property
    def fuzzer(self):
        """The most recently created FakeFuzzer object."""
        assert self._fuzzer, 'No fuzzer created.'
        return self._fuzzer

    # Unit test utilities

    def _ssh_cmd(self, args):
        """Returns the command line arguments for an SSH commaned."""
        return ['ssh'] + self.device.ssh_opts() + [self.device.addr] + args

    def _scp_cmd(self, args):
        return ['scp'] + self.device.ssh_opts() + args

    def get_process(self, args, ssh=False):
        cmd = self._ssh_cmd(args) if ssh else args
        return self.cli.create_process(cmd)

    def parse_args(self, *args):
        return self.parser.parse_args(args)

    def create_fuzzer(self, *args):
        args = self.parse_args(*args)
        self._fuzzer = self.factory.create_fuzzer(args)
        return self.fuzzer

    def set_outputs(
            self, args, outputs, start=None, end=None, reset=True, ssh=False):
        """Sets what will be returned from the stdout of a fake process.

        Providing a start and/or end will schedule the output to be added and/or
        removed, respectively, at a later time; see FakeProcess.schedule.
        Setting reset to True will replace any e4xisting output for the command.
        Setting ssh to true will automatically add the necessary SSH arguments.
        """
        process = self.get_process(args, ssh=ssh)
        if reset:
            process.clear()
        process.schedule('\n'.join(outputs), start=start, end=end)

    def set_running(self, package, executable, refresh=True, duration=None):
        """Marks a packaged executable as running on device.

        If refresh is True, this will cause the device to refresh its PIDs.
        If a duration is provided, the package executable will stop running
        after the given duration.
        """
        pid = self._next_pid
        self._next_pid += 1

        cmd = ['cs']
        output = '  {}.cmx[{}]: fuchsia-pkg://fuchsia.com/{}#meta/{}.cmx'.format(
            executable, pid, package, executable)

        end = None
        if duration:
            end = self.cli.elapsed + duration
        self.set_outputs(cmd, [output], end=end, reset=False, ssh=True)

        if refresh:
            self.device._pids = None

        return pid

    # Unit test assertions

    def assertLogged(self, *logs):
        """Asserts logs were generated by calls to cli.echo or cli.error."""
        self.assertEqual(self.cli.log, list(logs))

    def assertError(self, expr, *logs):
        with self.assertRaises(SystemExit):
            expr()
        self.assertLogged(*logs)

    def assertRan(self, *args):
        """Asserts a previous call was made to cli.create_process."""
        self.assertIn(' '.join(args), self.cli.processes.keys())

    def assertInputs(self, args, inputs):
        process = self.get_process(args)
        self.assertEqual(process.inputs, inputs)

    def assertScpTo(self, *args):
        """Asserts a previous call was made to device.scp with args."""
        args = list(args)[:-1] + [self.device.scp_rpath(args[-1])]
        cmd = self._scp_cmd(args)
        self.assertRan(*cmd)

    def assertScpFrom(self, *args):
        """Asserts a previous call was made to device.scp with args."""
        args = [self.device.scp_rpath(arg) for arg in args[:-1]] + [args[-1]]
        cmd = self._scp_cmd(args)
        self.assertRan(*cmd)

    def assertSsh(self, *args):
        """Asserts a previous call was made to device.ssh with cmd."""
        cmd = self._ssh_cmd(list(args))
        self.assertRan(*cmd)
