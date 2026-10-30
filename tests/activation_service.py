# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libwakeguard contributors
"""A fake login1 service activated only by the test's private session bus."""
import os
from gi.repository import Gio, GLib

XML = '''<node><interface name="org.freedesktop.login1.Manager"><method name="Inhibit">
<arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="h" direction="out"/></method></interface></node>'''
connection = Gio.DBusConnection.new_for_address_sync(os.environ['DBUS_STARTER_ADDRESS'],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
    None, None)
connection.set_exit_on_close(False)
readers = []


def inhibit(conn, sender, path, interface, method, params, invocation):
    reader, writer = os.pipe()
    readers.append(reader)
    fds = Gio.UnixFDList.new()
    index = fds.append(writer)
    os.close(writer)
    invocation.return_value_with_unix_fd_list(GLib.Variant('(h)', (index,)), fds)


connection.register_object('/org/freedesktop/login1',
    Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0], inhibit, None, None)
loop = GLib.MainLoop()
connection.connect('closed', lambda *_: loop.quit())
connection.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
    'RequestName', GLib.Variant('(su)', ('org.freedesktop.login1', 0)),
    GLib.VariantType.new('(u)'), Gio.DBusCallFlags.NONE, 2000, None)
loop.run()
for reader in readers:
    os.close(reader)
