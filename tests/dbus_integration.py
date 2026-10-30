# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libwakeguard contributors
"""Private-bus protocol tests; never touches the host power manager."""
import os
from pathlib import Path
import selectors
import signal
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from gi.repository import Gio, GLib

PROBE = sys.argv.pop(1)
os.environ['DBUS_SYSTEM_BUS_ADDRESS'] = os.environ['DBUS_SESSION_BUS_ADDRESS']
XML = '''<node>
<interface name="org.freedesktop.login1.Manager">
<method name="Inhibit"><arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="h" direction="out"/></method>
</interface>
<interface name="org.freedesktop.ConsoleKit.Manager">
<method name="Inhibit"><arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="h" direction="out"/></method>
</interface>
<interface name="org.freedesktop.portal.Inhibit">
<method name="Inhibit"><arg type="s" direction="in"/><arg type="u" direction="in"/>
<arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
</interface></node>'''
NAMES = ['org.freedesktop.login1', 'org.freedesktop.ConsoleKit', 'org.freedesktop.portal.Desktop']
PATHS = ['/org/freedesktop/login1', '/org/freedesktop/ConsoleKit/Manager', '/org/freedesktop/portal/desktop']


class Service:
    def __init__(self, address=None):
        self.address = address or os.environ['DBUS_SESSION_BUS_ADDRESS']
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self.run)
        self.calls = []
        self.readers = []
        self.pending = []
        self.behaviour = {}
        self.portal_senders = []
        self.senders = []
        self.thread.start()
        if not self.ready.wait(5):
            raise RuntimeError('mock service did not start')

    def bus_call(self, method, params, reply):
        return self.connection.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', method, params, GLib.VariantType.new(reply),
            Gio.DBusCallFlags.NONE, 2000, None)

    def name(self, index, acquire):
        if acquire:
            self.bus_call('RequestName', GLib.Variant('(su)', (NAMES[index], 0)), '(u)')
        else:
            self.bus_call('ReleaseName', GLib.Variant('(s)', (NAMES[index],)), '(u)')

    def run(self):
        self.context = GLib.MainContext.new()
        self.context.push_thread_default()
        self.connection = Gio.DBusConnection.new_for_address_sync(
            self.address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None, None)
        self.connection.set_exit_on_close(False)
        node = Gio.DBusNodeInfo.new_for_xml(XML)
        for index, interface in enumerate(node.interfaces):
            self.connection.register_object(PATHS[index], interface, self.method, None, None)
            self.name(index, True)
        self.loop = GLib.MainLoop.new(self.context, False)
        self.ready.set()
        self.loop.run()
        if not self.connection.is_closed():
            self.connection.close_sync(None)
        self.context.pop_thread_default()

    def method(self, connection, sender, path, interface, method, params, invocation):
        self.calls.append((interface, params.unpack()))
        self.senders.append(sender)
        behaviour = self.behaviour.get(interface, 'success')
        if behaviour == 'deny':
            invocation.return_dbus_error('org.freedesktop.DBus.Error.AccessDenied',
                                         'Test policy denies inhibition')
            return
        if behaviour == 'hang':
            self.pending.append(invocation)
            return
        if interface == 'org.freedesktop.portal.Inhibit':
            handle = '/org/freedesktop/portal/desktop/request/test/token'
            self.portal_senders.append(sender)
            # Intentionally emit before method reply to exercise subscription ordering.
            if behaviour != 'silent':
                response = GLib.Variant('(ua{sv})',
                    (1 if behaviour == 'cancel' else 2 if behaviour == 'refuse' else 0, {}))
                if behaviour == 'malformed_signal':
                    response = GLib.Variant('(s)', ('not a response',))
                signal_path = handle + '_other' if behaviour == 'wrong_path' else handle
                connection.emit_signal(sender, signal_path, 'org.freedesktop.portal.Request',
                    'Response', response)
            invocation.return_value(GLib.Variant('(o)', (handle,)))
            return
        fds = Gio.UnixFDList.new()
        if behaviour in ('malformed', 'negative_fd', 'missing_fds'):
            index = -1 if behaviour == 'negative_fd' else 0 if behaviour == 'missing_fds' else 8
            invocation.return_value_with_unix_fd_list(GLib.Variant('(h)', (index,)), fds)
            return
        reader, writer = os.pipe2(os.O_CLOEXEC | os.O_NONBLOCK)
        self.readers.append(reader)
        # The valid FD is index 1, not index 0. A client must use the reply handle.
        dummy = os.open('/dev/null', os.O_RDONLY)
        fds.append(dummy)
        os.close(dummy)
        index = fds.append(writer)
        os.close(writer)
        invocation.return_value_with_unix_fd_list(GLib.Variant('(h)', (index,)), fds)

    def owner_present(self, name):
        return self.bus_call('NameHasOwner', GLib.Variant('(s)', (name,)), '(b)').unpack()[0]

    def close(self):
        self.loop.quit()
        self.thread.join(5)
        if self.thread.is_alive():
            raise RuntimeError('mock service thread stuck')
        for reader in self.readers:
            os.close(reader)
        self.pending.clear()


def eventually(predicate, seconds=3):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(.01)
    return False


def fd_released(reader):
    try:
        return os.read(reader, 1) == b''
    except BlockingIOError:
        return False


class Integration(unittest.TestCase):
    def setUp(self):
        self.service = Service()
        self.processes = []

    def tearDown(self):
        failures = []
        try:
            for process in self.processes:
                if process.stdin:
                    try:
                        process.stdin.close()  # EOF releases guards through normal destructors.
                    except BrokenPipeError:
                        pass
                    process.stdin = None
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                    failures.append('client failed to exit after stdin EOF')
                stdout, stderr = process.communicate(timeout=5)
                expected = getattr(process, 'expected_returncode', None)
                if expected is not None:
                    if process.returncode != expected:
                        failures.append(f'client exit {process.returncode}, expected {expected}: {stderr}')
            self.assertFalse(failures, '\n'.join(failures))
        finally:
            self.service.close()

    def read(self, process):
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            self.assertTrue(selector.select(5), 'client did not respond within 5 seconds')
        line = process.stdout.readline().strip()
        if not line:
            process.wait(timeout=5)
            self.fail(f'client exited with {process.returncode}: {process.stderr.read()}')
        return line

    def start(self, backend=3, broad=0, timeout=1000, environment=None):
        env = os.environ.copy()
        if environment:
            env.update(environment)
        process = subprocess.Popen([PROBE, str(backend), str(broad), str(timeout)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env=env)
        self.processes.append(process)
        line = self.read(process)
        process.expected_returncode = 1 if line.startswith('ERROR ') else 0
        return process, line

    def command(self, process, command):
        process.stdin.write(command + '\n')
        process.stdin.flush()
        return self.read(process)

    def test_logind_lifetime_and_move(self):
        process, line = self.start()
        self.assertEqual(line, 'OK logind 0')
        self.assertEqual(self.service.calls[0][1],
            ('idle', 'test-application', 'Transferring test files', 'block'))
        self.assertFalse(fd_released(self.service.readers[0]))
        self.assertEqual(self.command(process, 'check'), 'CHECK 0')
        self.assertEqual(self.command(process, 'release'), 'RELEASED')
        self.assertTrue(eventually(lambda: fd_released(self.service.readers[0])))

    def test_independent_guards(self):
        first, line = self.start()
        self.assertEqual(line, 'OK logind 0')
        second, line = self.start()
        self.assertEqual(line, 'OK logind 0')
        self.command(first, 'release')
        self.assertTrue(eventually(lambda: fd_released(self.service.readers[0])))
        self.assertFalse(fd_released(self.service.readers[1]))
        self.assertEqual(self.command(second, 'check'), 'CHECK 0')

    def test_crash_releases_fd(self):
        process, line = self.start()
        self.assertEqual(line, 'OK logind 0')
        process.expected_returncode = -signal.SIGKILL
        process.kill()
        process.wait(timeout=5)
        self.assertTrue(eventually(lambda: fd_released(self.service.readers[0])))

    def test_consolekit_fallback(self):
        self.service.name(0, False)
        process, line = self.start(backend=0)
        self.assertEqual(line, 'OK consolekit 0')
        self.assertEqual(self.service.calls[0][0], 'org.freedesktop.ConsoleKit.Manager')

    def test_permission_denial_has_useful_detail(self):
        self.service.behaviour['org.freedesktop.login1.Manager'] = 'deny'
        process, line = self.start()
        self.assertIn('ERROR 3 logind:', line)
        self.assertIn('Test policy denies inhibition', line)

    def test_malformed_fd_fails(self):
        self.service.behaviour['org.freedesktop.login1.Manager'] = 'malformed'
        process, line = self.start()
        self.assertIn('invalid file-descriptor index', line)

    def test_negative_and_missing_fd(self):
        for behaviour in ('negative_fd', 'missing_fds'):
            with self.subTest(behaviour=behaviour):
                self.service.behaviour['org.freedesktop.login1.Manager'] = behaviour
                process, line = self.start()
                self.assertIn('invalid file-descriptor index', line)
                self.assertTrue(eventually(lambda:
                    not self.service.owner_present(self.service.senders[-1])))

    def test_explicit_backend_never_falls_back(self):
        self.service.behaviour['org.freedesktop.login1.Manager'] = 'deny'
        process, line = self.start(backend=3, broad=1)
        self.assertIn('ERROR 3 logind:', line)
        self.assertEqual(len(self.service.calls), 1)

    def test_broader_logind_and_consolekit_are_explicit(self):
        for backend, name in ((3, 'logind'), (4, 'consolekit')):
            with self.subTest(backend=name):
                process, line = self.start(backend=backend, broad=1)
                self.assertEqual(line, f'OK {name} 1')
                self.assertEqual(self.service.calls[-1][1],
                    ('sleep:idle', 'test-application', 'Transferring test files', 'block'))

    def test_explicit_consolekit(self):
        process, line = self.start(backend=4)
        self.assertEqual(line, 'OK consolekit 0')
        self.assertEqual(self.service.calls[0][1],
            ('idle', 'test-application', 'Transferring test files', 'block'))

    def test_denied_logind_falls_back_to_consolekit(self):
        self.service.behaviour['org.freedesktop.login1.Manager'] = 'deny'
        process, line = self.start(backend=0)
        self.assertEqual(line, 'OK consolekit 0')
        self.assertEqual(len(self.service.calls), 2)

    def test_malformed_logind_falls_back_and_closes_connection(self):
        self.service.behaviour['org.freedesktop.login1.Manager'] = 'malformed'
        process, line = self.start(backend=0)
        self.assertEqual(line, 'OK consolekit 0')
        self.assertTrue(eventually(lambda:
            not self.service.owner_present(self.service.senders[0])))

    def test_same_process_replacement_and_idempotent_reset(self):
        process, line = self.start()
        self.assertEqual(line, 'OK logind 0')
        self.assertEqual(self.command(process, 'replace'), 'REPLACED')
        self.assertEqual(len(self.service.readers), 2)
        self.assertTrue(eventually(lambda: fd_released(self.service.readers[0])))
        self.assertFalse(fd_released(self.service.readers[1]))
        self.assertEqual(self.command(process, 'reset'), 'RESET')
        self.assertTrue(eventually(lambda: fd_released(self.service.readers[1])))
        self.assertEqual(self.command(process, 'reset'), 'RESET')
        self.assertIn('CHECK 1 Guard is NULL', self.command(process, 'check'))

    def test_eight_parallel_guards_in_one_process(self):
        process, line = self.start(environment={'WG_TEST_PARALLEL': '1'})
        self.assertEqual(line, 'PARALLEL OK')
        self.assertEqual(len(self.service.readers), 8)
        self.assertTrue(eventually(lambda:
            all(fd_released(reader) for reader in self.service.readers)))

    def test_invalid_utf8_never_reaches_service(self):
        process, line = self.start(environment={'WG_TEST_INVALID_UTF8': '1'})
        self.assertIn('Options must contain valid UTF-8', line)
        self.assertFalse(self.service.calls)

    def test_unicode_metadata_and_portal_window(self):
        process, line = self.start(environment={'WG_TEST_UNICODE': '1'})
        self.assertEqual(line, 'OK logind 0')
        self.assertEqual(self.service.calls[0][1], ('idle', '下載器', '正在傳輸檔案', 'block'))
        portal, line = self.start(backend=2, broad=1, environment={'WG_TEST_UNICODE': '1'})
        self.assertEqual(line, 'OK portal 1')
        self.assertEqual(self.service.calls[1][1][0], 'x11:1234')
        self.assertEqual(self.service.calls[1][1][2]['reason'], '正在傳輸檔案')

    def test_unreachable_bus(self):
        process, line = self.start(environment={
            'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=/nonexistent/wakeguard-test-bus'})
        self.assertIn('ERROR 3 logind:', line)
        self.assertFalse(self.service.calls)

    def test_bus_disconnection_is_detected_without_main_loop(self):
        daemon = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        private_service = None
        try:
            address = daemon.stdout.readline().strip()
            self.assertTrue(address.startswith('unix:'), 'private bus failed to start')
            private_service = Service(address)
            process, line = self.start(environment={'DBUS_SYSTEM_BUS_ADDRESS': address})
            self.assertEqual(line, 'OK logind 0')
            daemon.terminate()
            daemon.wait(timeout=5)
            self.assertTrue(eventually(lambda: private_service.connection.is_closed()))
            self.assertTrue(eventually(lambda:
                'CHECK 5 Inhibitor bus connection was closed' in self.command(process, 'check')))
        finally:
            if private_service:
                private_service.close()
            if daemon.poll() is None:
                daemon.terminate()
            daemon.communicate(timeout=5)

    def test_installed_but_inactive_service_is_activated(self):
        with tempfile.TemporaryDirectory(prefix='wakeguard-activation-') as directory:
            service_dir = Path(directory) / 'dbus-1' / 'services'
            service_dir.mkdir(parents=True)
            helper = Path(__file__).with_name('activation_service.py')
            executable = shlex.quote(sys.executable) + ' ' + shlex.quote(str(helper))
            (service_dir / 'org.freedesktop.login1.service').write_text(
                '[D-BUS Service]\nName=org.freedesktop.login1\nExec=' + executable + '\n')
            env = os.environ.copy()
            env['XDG_DATA_HOME'] = directory
            daemon = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            try:
                address = daemon.stdout.readline().strip()
                self.assertTrue(address.startswith('unix:'))
                process, line = self.start(timeout=3000, environment={
                    'DBUS_SYSTEM_BUS_ADDRESS': address})
                self.assertEqual(line, 'OK logind 0')
                self.assertEqual(self.command(process, 'check'), 'CHECK 0')
                self.command(process, 'release')
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                daemon.communicate(timeout=5)

    def test_authentication_is_inside_total_deadline(self):
        with tempfile.TemporaryDirectory(prefix='wakeguard-auth-') as directory:
            path = str(Path(directory) / 'bus')
            stop = threading.Event()
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(path)
                listener.listen(1)
                listener.settimeout(5)
                def stall_authentication():
                    connection, _ = listener.accept()
                    with connection:
                        stop.wait(5)
                worker = threading.Thread(target=stall_authentication)
                worker.start()
                try:
                    start = time.monotonic()
                    process, line = self.start(timeout=120, environment={
                        'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=' + path})
                    self.assertIn('deadline exceeded', line)
                    self.assertLess(time.monotonic() - start, 1.5)
                finally:
                    stop.set()
                    worker.join(6)
                    self.assertFalse(worker.is_alive())

    def test_native_backend_unavailable_on_unix(self):
        process, line = self.start(backend=1)
        self.assertIn('Native backend is unavailable on Unix', line)
        self.assertFalse(self.service.calls)

    def test_hung_service_total_deadline(self):
        self.service.behaviour['org.freedesktop.login1.Manager'] = 'hang'
        self.service.behaviour['org.freedesktop.ConsoleKit.Manager'] = 'hang'
        start = time.monotonic()
        process, line = self.start(backend=0, timeout=120)
        self.assertIn('deadline exceeded', line)
        self.assertLess(time.monotonic() - start, 1.5)
        self.assertEqual(len(self.service.calls), 2)  # both share the total budget

    def test_service_disappeared(self):
        process, line = self.start()
        self.assertEqual(line, 'OK logind 0')
        self.service.name(0, False)
        self.assertIn('CHECK 5', self.command(process, 'check'))

    def test_service_replaced(self):
        process, line = self.start()
        self.assertEqual(line, 'OK logind 0')
        self.service.name(0, False)
        replacement = Gio.DBusConnection.new_for_address_sync(
            os.environ['DBUS_SESSION_BUS_ADDRESS'],
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None, None)
        try:
            replacement.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                'org.freedesktop.DBus', 'RequestName', GLib.Variant('(su)', (NAMES[0], 0)),
                GLib.VariantType.new('(u)'), Gio.DBusCallFlags.NONE, 2000, None)
            self.assertIn('CHECK 5 Inhibitor service restarted', self.command(process, 'check'))
        finally:
            replacement.close_sync(None)

    def test_missing_services_reports_each_attempt(self):
        self.service.name(0, False)
        self.service.name(1, False)
        process, line = self.start(backend=0)
        self.assertIn('logind:', line)
        self.assertIn('consolekit:', line)
        self.assertIn('ERROR 3', line)

    def test_strict_auto_never_calls_portal(self):
        process, line = self.start(backend=0)
        self.assertEqual(line, 'OK logind 0')
        self.assertTrue(all(call[0] != 'org.freedesktop.portal.Inhibit'
                            for call in self.service.calls))

    def test_portal_explicit_opt_in(self):
        process, line = self.start(backend=2)
        self.assertIn('allow_suspend_block=1', line)
        self.assertFalse(self.service.calls)

    def test_portal_confirmation_scope_and_disconnect(self):
        process, line = self.start(backend=0, broad=1)
        self.assertEqual(line, 'OK portal 1')
        interface, params = self.service.calls[0]
        self.assertEqual(params[1], 4)  # Suspend only, no screen locking/idle flag
        self.assertEqual(params[2]['reason'], 'Transferring test files')
        sender = self.service.portal_senders[0]
        self.assertTrue(self.service.owner_present(sender))
        self.command(process, 'release')
        self.assertTrue(eventually(lambda: not self.service.owner_present(sender)))

    def test_portal_refusal_falls_back(self):
        self.service.behaviour['org.freedesktop.portal.Inhibit'] = 'refuse'
        process, line = self.start(backend=0, broad=1)
        self.assertEqual(line, 'OK logind 1')
        self.assertEqual(self.service.calls[-1][1],
            ('sleep:idle', 'test-application', 'Transferring test files', 'block'))
        sender = self.service.portal_senders[0]
        self.assertTrue(eventually(lambda: not self.service.owner_present(sender)))

    def test_silent_portal_does_not_starve_logind_fallback(self):
        self.service.behaviour['org.freedesktop.portal.Inhibit'] = 'silent'
        start = time.monotonic()
        process, line = self.start(backend=0, broad=1, timeout=300)
        self.assertEqual(line, 'OK logind 1')
        self.assertEqual(self.service.calls[-1][1][0], 'sleep:idle')
        self.assertLess(time.monotonic() - start, 1.5)
        sender = self.service.portal_senders[0]
        self.assertTrue(eventually(lambda: not self.service.owner_present(sender)))

    def test_portal_refusal_is_not_success(self):
        self.service.behaviour['org.freedesktop.portal.Inhibit'] = 'refuse'
        process, line = self.start(backend=2, broad=1)
        self.assertIn('Portal refused inhibition (response 2)', line)

    def test_portal_cancel_is_not_success(self):
        self.service.behaviour['org.freedesktop.portal.Inhibit'] = 'cancel'
        process, line = self.start(backend=2, broad=1)
        self.assertIn('Portal refused inhibition (response 1)', line)

    def test_portal_denial_is_not_success(self):
        self.service.behaviour['org.freedesktop.portal.Inhibit'] = 'deny'
        process, line = self.start(backend=2, broad=1)
        self.assertIn('Test policy denies inhibition', line)

    def test_unrelated_or_malformed_portal_signals_are_ignored(self):
        for behaviour in ('wrong_path', 'malformed_signal'):
            with self.subTest(behaviour=behaviour):
                self.service.behaviour['org.freedesktop.portal.Inhibit'] = behaviour
                process, line = self.start(backend=2, broad=1, timeout=120)
                self.assertIn('did not confirm inhibition', line)

    def test_portal_service_disappearance(self):
        process, line = self.start(backend=2, broad=1)
        self.assertEqual(line, 'OK portal 1')
        self.service.name(2, False)
        self.assertIn('CHECK 5', self.command(process, 'check'))

    def test_portal_handle_without_confirmation_times_out(self):
        self.service.behaviour['org.freedesktop.portal.Inhibit'] = 'silent'
        start = time.monotonic()
        process, line = self.start(backend=2, broad=1, timeout=120)
        self.assertIn('did not confirm inhibition', line)
        self.assertLess(time.monotonic() - start, 1.5)
        sender = self.service.portal_senders[0]
        self.assertTrue(eventually(lambda: not self.service.owner_present(sender)))


if __name__ == '__main__':
    unittest.main(verbosity=2)
