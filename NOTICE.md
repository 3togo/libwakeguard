# Reference and attribution

libwakeguard is an independent C++ implementation under the MIT licence.

Behavioural reference: [segevfiner/keepawake-rs](https://github.com/segevfiner/keepawake-rs),
reviewed at commit `6e7aa927d504a0e03f46ae6f236330a6838a1cd2`.
Its separation of idle-system sleep from explicit sleep and display inhibition
informed the API contract. Its Rust implementation was read as a reference;
no Rust source is incorporated or linked. The original MIT notice is retained
in `third_party/keepawake-rs.LICENSE` as attribution.

The implementation also follows the public Windows power-request, Apple IOKit,
logind, ConsoleKit2 and XDG portal protocols. Their implementation source is not
incorporated. Unix builds link to the system's GIO/GLib libraries, which have
separate LGPL terms; those libraries are not vendored here.

MIT permits use, modification, linking, sublicensing and redistribution,
including within aMule (GPL-2.0-or-later), provided the applicable copyright and
licence notices are retained. Combining this library with aMule does not remove
aMule's GPL obligations. See `LICENSE` and
[the FSF's Expat/MIT compatibility entry](https://www.gnu.org/licenses/license-list.html#Expat).
