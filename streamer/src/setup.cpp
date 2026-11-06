#ifndef _WIN32 // Linux system check; the Windows checks are below
#include "app_paths.h"
#include "setup.h"
#include "settings.h"
#include "hardware_encoder.h"
#include "games.h"
#include "wineopenxr_fix.h"
#include "runtime_registration.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QIcon>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <unistd.h>

namespace
{
constexpr const char * udpPort = "38850";
constexpr const char * tcpRange = "38810:38840";

QString run(const QString & program, const QStringList & args, int * exitCode = nullptr)
{
	QProcess p;
	p.start(program, args);
	if (!p.waitForFinished(3000))
	{
		if (exitCode)
			*exitCode = -1;
		return {};
	}
	if (exitCode)
		*exitCode = p.exitCode();
	return QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError());
}

bool serviceActive(const QString & name)
{
	return run("systemctl", {"is-active", name}).trimmed() == "active";
}

// root fixes whose result can't be checked without root (ufw's rule files are root-only on Ubuntu/Debian)
QString appliedFixesPath()
{
	return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/OpenDesk/root-fixes";
}

bool fixApplied(const QString & key)
{
	QFile f(appliedFixesPath());
	return f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()).split('\n').contains(key);
}

// ufw keeps rules in files like "### tuple ### allow udp 38850 0.0.0.0/0 any 0.0.0.0/0 in comment=<hex>"
// (action proto dport dst sport src direction [comment]); ports may be lists and ranges ("80,38810:38840").
// true if every port of [first, last] is allowed in for proto; nullopt: the file is not readable
std::optional<bool> ufwAllows(const QString & file, const QString & proto, int first, int last)
{
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly))
		return std::nullopt;
	std::vector<bool> open(size_t(last - first + 1), false);
	for (const QString & line: QString::fromUtf8(f.readAll()).split('\n'))
	{
		if (!line.startsWith("### tuple ### "))
			continue;
		QStringList t = line.mid(14).split(' ', Qt::SkipEmptyParts);
		if (t.size() < 7 || t[0] != "allow" || (t[1] != proto && t[1] != "any") || t[6] != "in")
			continue; // also skips app profiles ("Samba - in") and interface-only rules ("in_virbr0")
		for (const QString & part: t[2].split(','))
		{
			QStringList r = part.split(':');
			bool ok1 = false, ok2 = true;
			int lo = r[0].toInt(&ok1), hi = r.size() > 1 ? r[1].toInt(&ok2) : lo;
			if (!ok1 || !ok2)
				continue;
			for (int p = std::max(lo, first); p <= std::min(hi, last); ++p)
				open[size_t(p - first)] = true;
		}
	}
	return std::all_of(open.begin(), open.end(), [](bool b) { return b; });
}

SetupItem firewall()
{
	SetupItem item{"Firewall allows the headset (udp 38850, tcp 38810-38840)"};
	if (serviceActive("ufw"))
	{
		item.rootFix = {QString("ufw allow %1/udp comment 'OpenDesk'").arg(udpPort),
		                QString("ufw allow %1/tcp comment 'OpenDesk'").arg(tcpRange)};
		auto v4u = ufwAllows("/etc/ufw/user.rules", "udp", 38850, 38850), v4t = ufwAllows("/etc/ufw/user.rules", "tcp", 38810, 38840);
		auto v6u = ufwAllows("/etc/ufw/user6.rules", "udp", 38850, 38850), v6t = ufwAllows("/etc/ufw/user6.rules", "tcp", 38810, 38840);
		if (!v4u || !v6u)
		{
			item.fixedKey = "ufw";
			item.ok = fixApplied(item.fixedKey);
			item.detail = item.ok ? "ufw: rules added (ufw's rule files are only readable by root)"
			                      : "ufw is active; its rules are only readable by root - the fix adds them";
			return item;
		}
		bool v4 = *v4u && *v4t, v6 = *v6u && v6t.value_or(false);
		item.ok = v4 && v6;
		item.detail = item.ok ? "ufw: rules present (IPv4 + IPv6)"
		                      : QString("ufw is active and is missing rules for %1").arg(!v4 && !v6 ? "IPv4 and IPv6" : !v4 ? "IPv4" : "IPv6 (USB connection)");
		return item;
	}
	if (serviceActive("firewalld"))
	{
		// every zone with an interface or source (Wi-Fi is often in "home"/"trusted", not the default zone)
		QStringList zones{run("firewall-cmd", {"--get-default-zone"}).trimmed()};
		for (const QString & line: run("firewall-cmd", {"--get-active-zones"}).split('\n'))
			if (!line.isEmpty() && !line.front().isSpace() && !line.contains(':') && !zones.contains(line.trimmed()))
				zones << line.trimmed();
		zones.removeAll(QString());
		QStringList closed;
		for (const QString & z: zones)
		{
			QString ports = run("firewall-cmd", {"--zone=" + z, "--list-ports"});
			if (!ports.contains("38850/udp") || !ports.contains("38810-38840/tcp"))
				closed << z;
		}
		item.ok = closed.isEmpty() && !zones.isEmpty();
		item.detail = item.ok ? "firewalld: ports open (zones " + zones.join(", ") + ")"
		                      : "firewalld is active and the ports are closed (zone " + (closed.isEmpty() ? QString("?") : closed.join(", ")) + ")";
		for (const QString & z: closed.isEmpty() ? QStringList{QString()} : closed)
			item.rootFix << QString("firewall-cmd --permanent %1--add-port=38850/udp --add-port=38810-38840/tcp").arg(z.isEmpty() ? "" : "--zone=" + z + " ");
		item.rootFix << "firewall-cmd --reload";
		return item;
	}
	if (serviceActive("nftables") || serviceActive("iptables"))
	{
		item.ok = false;
		item.detail = "a custom nftables/iptables firewall is active; allow udp 38850 and tcp 38810-38840 manually";
		return item;
	}
	item.ok = true;
	item.detail = "no active firewall service (ufw/firewalld/nftables) detected";
	return item;
}

SetupItem usbNetwork()
{
	// The Quest's USB link is a cdc_ncm network adapter. NetworkManager's automatic "Wired connection N" runs
	// DHCP on it, times out after 45 s, tears the link down (dropping the IPv6 link-local address the USB
	// stream uses) and retries forever: the stream freezes every ~45 s. A profile matched by driver with
	// IPv4 off and IPv6 link-local only keeps the link up.
	SetupItem item{"USB connection to the headset (NetworkManager)"};
	if (!serviceActive("NetworkManager"))
	{
		item.ok = true;
		item.detail = "NetworkManager not in use";
		return item;
	}
	QString profiles = run("nmcli", {"-t", "-f", "NAME", "connection", "show"});
	item.ok = profiles.split('\n').contains("OpenDesk USB");
	item.detail = item.ok ? "'OpenDesk USB' profile present (link-local only, no DHCP)"
	                      : "no profile for the USB link: NetworkManager retries DHCP on it and drops the stream every ~45 s";
	item.rootFix = {
	        "nmcli connection add type ethernet con-name 'OpenDesk USB' match.driver cdc_ncm ipv4.method disabled "
	        "ipv6.method link-local ipv6.addr-gen-mode eui64 connection.autoconnect yes connection.autoconnect-priority 100",
	        // switch an already plugged-in headset over to the new profile
	        "for i in /sys/class/net/*; do [ \"$(basename \"$(readlink -f \"$i/device/driver\" 2>/dev/null)\")\" = cdc_ncm ] && "
	        "nmcli connection up 'OpenDesk USB' ifname \"$(basename \"$i\")\" || true; done"};
	return item;
}

SetupItem uinput()
{
	SetupItem item{"Input from the headset (/dev/uinput)"};
	item.ok = access("/dev/uinput", W_OK) == 0;
	item.detail = item.ok ? "writable" : "not writable: mouse/keyboard from the headset won't work";
	// logind grants the active session user access to devices tagged uaccess
	item.rootFix = {"echo 'KERNEL==\"uinput\", SUBSYSTEM==\"misc\", TAG+=\"uaccess\", OPTIONS+=\"static_node=uinput\"' > "
	                "/etc/udev/rules.d/60-opendesk-uinput.rules",
	                "udevadm control --reload-rules", "udevadm trigger --name-match=uinput"};
	return item;
}

SetupItem capture()
{
	// X11 sessions: grabbed directly (MIT-SHM), no portal involved
	if (qEnvironmentVariable("XDG_SESSION_TYPE") == "x11")
	{
		SetupItem item{"Desktop capture (X11)"};
		item.ok = true;
		item.detail = "X11 session: monitors from XRandR, captured with MIT-SHM (no virtual monitors)";
		return item;
	}
	// xdg-desktop-portal ScreenCast; v4+ supports restore tokens (the share dialog is shown only once)
	SetupItem item{"Desktop capture (screen-share portal)"};
	QString out = run("busctl", {"--user", "get-property", "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
	                             "org.freedesktop.portal.ScreenCast", "version"});
	int version = out.section(' ', 1, 1).trimmed().toInt();
	item.ok = version >= 4;
	item.detail = version >= 4 ? QString("ScreenCast portal v%1 (permission is remembered after the first approval)").arg(version)
	              : version > 0 ? QString("ScreenCast portal v%1 is too old: the share dialog would appear every time").arg(version)
	                            : "no ScreenCast portal: install xdg-desktop-portal and the one for your desktop (-kde, -gnome, -wlr)";
	return item;
}

QString grantDesktopFile()
{
	return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/applications/opendesk-streamer.desktop";
}

// the launcher icon: the app icon as a PNG in the user's icon theme
void installLauncherIcon()
{
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/icons/hicolor/256x256/apps";
	const QString file = dir + "/opendesk-streamer.png";
	QIcon icon(":/assets/opendesk/app.ico");
	if (icon.isNull() || QFileInfo::exists(file))
		return;
	QDir().mkpath(dir);
	icon.pixmap(256, 256).save(file, "PNG");
}

QString grantDesktopEntry()
{
	QString exe = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
	return QString("[Desktop Entry]\n"
	               "Type=Application\n"
	               "Name=OpenDesk Streamer\n"
	               "GenericName=VR Streamer\n"
	               "Comment=OpenDesk Streamer\n"
	               "Exec=\"%1\"\n"
	               "Icon=opendesk-streamer\n"
	               "Terminal=false\n"
	               "Categories=Game;\n"
	               "Keywords=VR;Quest;OpenDesk;Streamer;OpenXR;\n"
	               "StartupWMClass=opendesk-streamer\n"
	               "# lets KWin hand this program its screen-cast protocol: every monitor without a dialog, virtual monitors\n"
	               "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1\n")
	        .arg(exe);
}

SetupItem kwinCapture()
{
	// KWin gives its private screen-cast protocol (what its own portal and krdp use) to programs whose .desktop
	// entry lists it; with it: no share dialog, each monitor its own stream, virtual monitors for the headset
	SetupItem item{"KWin screen capture (multi-monitor, virtual monitors)"};
	// granted = our entry lists the interface for this executable; its name/comment lines don't matter (and are
	// refreshed quietly after a rename)
	QFile f(grantDesktopFile());
	QString current = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
	f.close();
	QString execLine = grantDesktopEntry().section("Exec=", 1).section('\n', 0, 0);
	item.ok = current.contains("Exec=" + execLine + "\n") && current.contains("X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1");
	if (item.ok)
		installLauncherIcon();
	if (item.ok && current != grantDesktopEntry() && f.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		f.write(grantDesktopEntry().toUtf8());
		f.close();
		QProcess::startDetached("kbuildsycoca6", {});
	}
	item.detail = item.ok ? "granted: every monitor streams without a dialog; Add monitor / Extra display create virtual monitors"
	                      : "not granted: the screen-share portal is used instead (monitors picked in its dialog, no virtual "
	                        "monitors). The fix installs a menu entry for this streamer that lets KWin allow screen capture "
	                        "without asking.";
	item.userFix = [] {
		QString path = grantDesktopFile();
		QDir().mkpath(QFileInfo(path).absolutePath());
		QFile out(path);
		if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
			return QString("cannot write %1").arg(path);
		out.write(grantDesktopEntry().toUtf8());
		out.close();
		installLauncherIcon();
		QProcess::execute("kbuildsycoca6", {}); // KWin looks the entry up through the KService cache
		return QString();
	};
	return item;
}

SetupItem audio()
{
	SetupItem item{"Audio (PipeWire / PulseAudio)"};
	int code = 0;
	QString out = run("pactl", {"info"}, &code);
	item.ok = code == 0;
	item.detail = item.ok ? out.section("Server Name: ", 1, 1).section('\n', 0, 0) : "no audio server reachable (pactl info failed)";
	return item;
}

SetupItem encoder()
{
	SetupItem item{"Hardware video encoder (automatic)"};
	auto probe = [](const std::string & node) {
		std::string err = od::hardware_encoder_probe(node);
		if (!err.empty())
			return std::pair{false, QString::fromStdString(err)};
		QStringList codecs;
		for (od::video_codec c: od::hardware_encoder_codecs(node))
			codecs << od::codec_name(c);
		return std::pair{true, QString("%1 on %2 (%3)").arg(od::gpu_vendor(node) == 0x10de ? "NVENC" : "VAAPI", QString::fromStdString(node),
		                                                  codecs.join(", "))};
	};
	const std::string desktop = od::default_render_node(), vr = od::vr_render_node();
	auto [ok, detail] = probe(desktop);
	item.ok = ok;
	item.detail = detail;
	if (vr != desktop) // hybrid laptop: VR on the discrete GPU
	{
		auto [vr_ok, vr_detail] = probe(vr);
		item.ok &= vr_ok;
		item.detail = "desktop: " + detail + "; VR: " + vr_detail;
	}
	return item;
}
} // namespace

// the Proton RiftLift launches Oculus games with (`riftlift proton`: "Current: <selection>" then the resolved path)
QString riftliftProton(const QString & riftlift)
{
	QProcess p;
	p.start(riftlift, {"proton"});
	if (!p.waitForFinished(15000))
		return {};
	QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
	return lines.isEmpty() || !QFileInfo(lines.last().trimmed()).isAbsolute() ? QString() : lines.last().trimmed();
}

QString wineopenxrDll(const QString & proton)
{
	return proton + "/files/lib/wine/x86_64-windows/wineopenxr.dll";
}

SetupItem riftliftWineOpenXR(const QString & riftlift)
{
	// Proton's WineOpenXR keeps destroyed sessions in its session list (use-after-free): RiftLift's bridge creates and
	// destroys probe sessions, so Oculus games crash ~30 s in. Fixed upstream in Proton 11 / GE-Proton 11; builds without
	// the fix (e.g. proton-cachyos 11) get a 74-byte patch of wineopenxr.dll (od::patch_wineopenxr)
	SetupItem item{"RiftLift: Oculus games under Proton (WineOpenXR fix)"};
	QString proton = riftliftProton(riftlift);
	if (proton.isEmpty())
	{
		item.ok = true;
		item.detail = "could not ask RiftLift which Proton it uses";
		return item;
	}
	std::string detail;
	auto state = od::check_wineopenxr(wineopenxrDll(proton).toStdString(), &detail);
	QString name = QFileInfo(proton).fileName();
	item.ok = state != od::wineopenxr_state::needs_patch;
	item.detail = QString("%1: %2").arg(name, QString::fromStdString(detail));
	if (state == od::wineopenxr_state::needs_patch)
		item.detail += QFileInfo(QFileInfo(wineopenxrDll(proton)).absolutePath()).isWritable()
		                       ? ". The fix patches its wineopenxr.dll (the original is kept as wineopenxr.dll.vd-orig)."
		                       : ". The fix copies this system-installed Proton to RiftLift's tools folder, patches the copy's "
		                         "wineopenxr.dll and selects it in RiftLift; the installed one stays untouched.";
	if (state != od::wineopenxr_state::needs_patch)
		return item;
	item.userFix = [riftlift, proton, name]() -> QString {
		QString target = proton;
		if (!QFileInfo(QFileInfo(wineopenxrDll(proton)).absolutePath()).isWritable())
		{
			// system package: a copy (reflinked where the file system can) in RiftLift's tools folder
			QString tools = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/riftlift/tools";
			target = tools + "/" + name + "-vdfix";
			if (!QFileInfo::exists(target + "/proton"))
			{
				QDir().mkpath(tools);
				QProcess cp;
				cp.start("cp", {"-a", "--reflink=auto", proton, target + ".partial"});
				if (!cp.waitForFinished(-1) || cp.exitCode() != 0)
					return QString("copying %1 failed: %2").arg(proton, QString::fromUtf8(cp.readAllStandardError()).trimmed());
				if (!QDir().rename(target + ".partial", target))
					return QString("cannot rename the copy to %1").arg(target);
			}
		}
		else if (!QFileInfo::exists(wineopenxrDll(proton) + ".vd-orig"))
			QFile::copy(wineopenxrDll(proton), wineopenxrDll(proton) + ".vd-orig"); // keep the original next to it
		QString dll = wineopenxrDll(target), dir = QFileInfo(dll).absolutePath();
		// the copy keeps the package's read-only modes: the patch is written next to the DLL and renamed over it
		QFile::setPermissions(dir, QFile::permissions(dir) | QFileDevice::WriteOwner);
		QFile::setPermissions(dll, QFile::permissions(dll) | QFileDevice::WriteOwner);
		std::string error;
		if (!od::patch_wineopenxr(dll.toStdString(), &error))
			return QString("patching %1 failed: %2").arg(dll, QString::fromStdString(error));
		if (target != proton)
		{
			QProcess sel;
			sel.start(riftlift, {"proton", target});
			if (!sel.waitForFinished(15000) || sel.exitCode() != 0)
				return QString("patched %1, but selecting it in RiftLift failed: %2")
				        .arg(target, QString::fromUtf8(sel.readAllStandardError()).trimmed());
		}
		return QString();
	};
	return item;
}

SetupItem xrizer()
{
	// OpenVR games (most of Steam's VR library) run on OpenDesk XR through xrizer, OpenVR implemented on OpenXR.
	// Only our own copy is used (tools/get_xrizer.sh: xrizer's CI nightly, or a source build as fallback); the streamer
	// also fetches it by itself when VR starts without one.
	SetupItem item{"OpenVR games (xrizer)"};
	QString version = QString::fromStdString(od::openvr_runtime::xrizer_version());
	item.ok = !od::openvr_runtime::find_xrizer().empty();
	item.detail = item.ok ? QString("xrizer (%1) in %2: OpenVR games (Half-Life: Alyx, Beat Saber, VRChat, ...) run on "
	                                "OpenDesk XR while streaming")
	                                .arg(version.isEmpty() ? "OPENDESK_XRIZER" : version,
	                                     QString::fromStdString(od::openvr_runtime::find_xrizer()))
	                      : "not installed: OpenVR games need it to run on OpenDesk XR. The fix downloads xrizer's nightly build "
	                        "(github.com/Supreeeme/xrizer CI), or builds it from source if that fails (git + Rust).";
	item.userFix = [] {
		auto run = [](const QStringList & args, QString & out) {
			QProcess p;
			p.setProcessChannelMode(QProcess::MergedChannels);
			p.start("sh", QStringList{od::resource_dir("tools", OPENDESK_TOOLS_DIR) + "/get_xrizer.sh"} + args);
			QEventLoop loop; // the dialog keeps painting meanwhile
			QObject::connect(&p, &QProcess::finished, &loop, &QEventLoop::quit);
			QObject::connect(&p, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
			QTimer::singleShot(30 * 60 * 1000, &loop, &QEventLoop::quit);
			if (p.state() != QProcess::NotRunning)
				loop.exec();
			bool ok = p.state() == QProcess::NotRunning && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
			p.kill();
			QStringList lines = QString::fromUtf8(p.readAll()).trimmed().split('\n');
			out = lines.mid(std::max<qsizetype>(0, lines.size() - 4)).join('\n');
			return ok;
		};
		QString nightly, source;
		if (run({"nightly"}, nightly) || run({"source"}, source))
			return QString();
		return QString("downloading xrizer failed:\n%1\nbuilding it from source failed too:\n%2").arg(nightly, source);
	};
	return item;
}

QList<SetupItem> runSetupChecks()
{
	QList<SetupItem> items{firewall(), usbNetwork(), uinput()};
	if (qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("KDE", Qt::CaseInsensitive))
		items << kwinCapture();
	items << capture() << encoder() << audio() << xrizer();
	if (QString riftlift = od::riftlift_executable(); !riftlift.isEmpty())
		items << riftliftWineOpenXR(riftlift);
	return items;
}

SetupDialog::SetupDialog(QWidget * parent) :
        QDialog(parent)
{
	setObjectName("vdDialog");
	setWindowTitle("OpenDesk Streamer - System check");
	setMinimumWidth(620);
	auto * v = new QVBoxLayout(this);

	auto * title = new QLabel("System check");
	title->setObjectName("platformName");
	auto * note = new QLabel("Checks what streaming to the headset needs on this system, and fixes what it can.");
	note->setObjectName("accountIds");
	note->setWordWrap(true);
	v->addWidget(title);
	v->addWidget(note);
	v->addSpacing(6);

	auto * host = new QWidget;
	list_ = new QVBoxLayout(host);
	list_->setContentsMargins(0, 0, 0, 0);
	v->addWidget(host);

	auto * startup = new QCheckBox("Check at startup");
	startup->setChecked(StreamerSettings::instance().checkSetupAtStartup);
	connect(startup, &QCheckBox::toggled, this, [](bool on) {
		StreamerSettings::instance().checkSetupAtStartup = on;
		StreamerSettings::instance().save();
	});

	auto * buttons = new QHBoxLayout;
	fixButton_ = new QPushButton("Fix automatically");
	fixButton_->setToolTip("Applies the fixes; asks for your password once if one needs root");
	auto * copy = new QPushButton("Copy commands");
	auto * close = new QPushButton("Close");
	connect(fixButton_, &QPushButton::clicked, this, &SetupDialog::fix);
	connect(copy, &QPushButton::clicked, this, [this] {
		QStringList cmds;
		for (const auto & item: items_)
			if (!item.ok)
				for (const QString & c: item.rootFix)
					cmds << "sudo " + c;
		QApplication::clipboard()->setText(cmds.join('\n'));
	});
	connect(close, &QPushButton::clicked, this, &QDialog::accept);
	buttons->addWidget(startup);
	buttons->addStretch();
	buttons->addWidget(copy);
	buttons->addWidget(fixButton_);
	buttons->addWidget(close);
	v->addLayout(buttons);
	refresh();
}

bool SetupDialog::hasProblems() const
{
	return std::any_of(items_.begin(), items_.end(), [](const SetupItem & i) { return !i.ok; });
}

void SetupDialog::refresh()
{
	while (QLayoutItem * item = list_->takeAt(0))
	{
		if (item->widget())
		{
			item->widget()->hide();
			item->widget()->deleteLater();
		}
		delete item;
	}
	items_ = runSetupChecks();
	bool fixable = false;
	for (const auto & item: items_)
	{
		auto * row = new QWidget;
		auto * h = new QHBoxLayout(row);
		h->setContentsMargins(0, 2, 0, 2);
		auto * icon = new QLabel;
		QPixmap pix(item.ok ? ":/assets/opendesk/info.png" : ":/assets/opendesk/warning.png");
		if (pix.isNull())
			icon->setText(item.ok ? "OK" : "!");
		else
			icon->setPixmap(pix);
		icon->setFixedWidth(24);
		auto * text = new QLabel(QString("<b>%1</b><br><span style=\"color:%2\">%3</span>")
		                                 .arg(item.title.toHtmlEscaped(), item.ok ? "#808080" : "#FFA500", item.detail.toHtmlEscaped()));
		text->setWordWrap(true);
		text->setObjectName("aboutValue");
		h->addWidget(icon, 0, Qt::AlignTop);
		h->addWidget(text, 1);
		list_->addWidget(row);
		fixable |= !item.ok && (!item.rootFix.isEmpty() || item.userFix);
	}
	fixButton_->setEnabled(fixable);
}

void SetupDialog::fix()
{
	QStringList cmds, errors;
	QApplication::setOverrideCursor(Qt::WaitCursor); // fixes may take a while (building xrizer)
	for (const auto & item: items_)
		if (!item.ok)
		{
			cmds += item.rootFix;
			if (item.userFix)
				if (QString e = item.userFix(); !e.isEmpty())
					errors << item.title + ": " + e;
		}
	QApplication::restoreOverrideCursor();
	if (!errors.isEmpty())
		QMessageBox::warning(this, windowTitle(), errors.join('\n'));
	if (cmds.isEmpty())
		return refresh();
	// one pkexec invocation = one password prompt from the desktop's polkit agent
	QProcess p;
	p.setProcessChannelMode(QProcess::MergedChannels);
	p.start("pkexec", {"sh", "-c", "set -e\n" + cmds.join('\n')});
	if (!p.waitForStarted())
	{
		QMessageBox::warning(this, windowTitle(), "pkexec (polkit) is not available. Use \"Copy commands\" and run them in a terminal.");
		return refresh();
	}
	p.waitForFinished(120000);
	QString output = QString::fromUtf8(p.readAll()).trimmed();
	if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
	{
		// 126/127 from pkexec: cancelled, not authorised, or no polkit agent; the output tells which
		QString why = p.exitCode() == 126 || p.exitCode() == 127 ? "Authorization was cancelled or denied, or a command is missing."
		                                                        : "Some commands failed.";
		QMessageBox::warning(this, windowTitle(),
		                     why + (output.isEmpty() ? "" : "\n\n" + output) + "\n\n\"Copy commands\" gives them for a terminal.");
	}
	else
	{
		QStringList keys;
		for (const auto & item: items_)
			if (!item.ok && !item.fixedKey.isEmpty())
				keys << item.fixedKey;
		QDir().mkpath(QFileInfo(appliedFixesPath()).absolutePath());
		QFile f(appliedFixesPath());
		if (!keys.isEmpty() && f.open(QIODevice::Append))
			f.write((keys.join('\n') + '\n').toUtf8());
	}
	refresh();
}

#else // _WIN32

#include "setup.h"
#include "settings.h"

#include "hardware_encoder.h"
#include "app_paths.h"
#include "dxvk_win.h"
#include "games.h"
#include "platform.h"
#include "runtime_registration.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>

namespace
{
SetupItem item(QString title, bool ok, QString detail)
{
	SetupItem i;
	i.title = std::move(title);
	i.ok = ok;
	i.detail = std::move(detail);
	return i;
}

SetupItem encoderItem()
{
	const std::string error = od::hardware_encoder_probe({});
	if (error.empty())
		return item("Hardware video encoder", true, "A H.264 hardware encoder is available (NVENC, AMF, QSV or Media Foundation)");
	SetupItem i = item("Hardware video encoder", false, QString::fromStdString(error));
	i.detail += "\nInstall the current GPU driver; without one the streamer falls back to slower software encoding.";
	return i;
}

QString firewallRuleName()
{
	return "OpenDesk Streamer";
}

SetupItem firewallItem()
{
	QProcess p;
	p.setProcessChannelMode(QProcess::MergedChannels);
	p.start("netsh", {"advfirewall", "firewall", "show", "rule", "name=" + firewallRuleName()});
	if (!p.waitForFinished(3000) || p.exitCode() != 0 ||
	    !QString::fromLocal8Bit(p.readAll()).contains(firewallRuleName(), Qt::CaseInsensitive))
	{
		SetupItem i = item("Windows Firewall", false,
		                   "No inbound rule for OpenDesk yet. The headset connects over TCP 38810-38840 and discovers the PC over UDP 38850.");
		i.userFix = [] {
			const std::wstring name = firewallRuleName().toStdWString();
			const std::wstring tcp = L"advfirewall firewall add rule name=\"" + name +
			                         L"\" dir=in action=allow protocol=TCP localport=38810-38840";
			const std::wstring udp =
			        L"advfirewall firewall add rule name=\"" + name + L" (UDP)\" dir=in action=allow protocol=UDP localport=38850";
			const HINSTANCE tcpResult = ShellExecuteW(nullptr, L"runas", L"netsh.exe", tcp.c_str(), nullptr, SW_HIDE);
			if (INT_PTR(tcpResult) <= 32)
				return QString("Windows did not accept the firewall rule (UAC was cancelled?)");
			const HINSTANCE udpResult = ShellExecuteW(nullptr, L"runas", L"netsh.exe", udp.c_str(), nullptr, SW_HIDE);
			return INT_PTR(udpResult) > 32 ? QString() : QString("Windows did not accept the UDP firewall rule (UAC was cancelled?)");
		};
		return i;
	}
	return item("Windows Firewall", true, "OpenDesk's inbound rules are present (TCP 38810-38840, UDP 38850)");
}

SetupItem audioItem()
{
	const UINT n = waveOutGetNumDevs();
	SetupItem i = item("Audio playback device", n > 0, n ? QString("%1 playback device(s)").arg(n)
	                                                      : "No audio output device was found");
	if (!n)
		i.detail += "\nEnable an output device in Windows Settings > System > Sound.";
	return i;
}

SetupItem libraryItem()
{
	const bool steam = !od::platform::steam_root().isEmpty();
	const bool oculus = QFileInfo::exists(od::oculus_client_executable());
	QStringList found;
	if (steam)
		found << "Steam";
	if (oculus)
		found << "Oculus";
	return item("Game libraries", true, found.isEmpty() ? "No Steam or Oculus installation found (desktop streaming still works)"
	                                                   : found.join(" and ") + " found");
}
// OpenDesk XR as the system's OpenXR runtime (HKLM: the installer sets it; else one elevated reg.exe)
SetupItem openxrItem()
{
	const QString manifest = QDir::toNativeSeparators(od::resource_dir("runtime", "") + "/opendesk_xr.json");
	if (!QFileInfo::exists(manifest))
		return item("OpenXR runtime", false, "OpenDesk XR is missing from this installation (" + manifest + "); reinstall OpenDesk");
	if (od::openxr_runtime::active())
		return item("OpenXR runtime", true, "OpenDesk XR is the active OpenXR runtime: OpenXR games render through OpenDesk");
	SetupItem i = item("OpenXR runtime", false,
	                   "Another OpenXR runtime is active, so OpenXR games won't stream. Fixing it makes OpenDesk XR the active "
	                   "runtime (Windows asks for administrator rights).");
	i.userFix = [manifest] {
		const std::wstring args = L"add \"HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\" /v ActiveRuntime /t REG_SZ /d \"" + manifest.toStdWString() +
		                          L"\" /f /reg:64";
		const HINSTANCE r = ShellExecuteW(nullptr, L"runas", L"reg.exe", args.c_str(), nullptr, SW_HIDE);
		return INT_PTR(r) > 32 ? QString() : QString("Windows did not change the OpenXR runtime (UAC was cancelled?)");
	};
	return i;
}

SetupItem openvrItem()
{
	const std::string xrizer = od::openvr_runtime::find_xrizer();
	if (xrizer.empty())
		return item("OpenVR games without SteamVR", false,
		            "xrizer is missing from this installation: OpenVR games need SteamVR. Reinstall OpenDesk.");
	return item("OpenVR games without SteamVR", true,
	            QString("xrizer (%1) runs OpenVR games on OpenDesk XR while the streamer runs")
	                    .arg(QString::fromStdString(od::openvr_runtime::xrizer_version().empty() ? xrizer : od::openvr_runtime::xrizer_version())));
}

SetupItem dxvkItem()
{
	const QString dir = od::dxvk::bundled_dir();
	QFile version(od::resource_dir("dxvk", "") + "/VERSION.txt");
	const QString v = version.open(QIODevice::ReadOnly) ? QString::fromUtf8(version.readAll()).trimmed() : QString();
	return item("DXVK", !dir.isEmpty(),
	            dir.isEmpty() ? "DXVK is missing from this installation: the per-game DXVK option (VR page) does nothing"
	                          : QString("DXVK %1 is available for the per-game DXVK option (VR page)").arg(v));
}
// optional: virtual monitors (Add monitor / Extra display) come from the Virtual Display Driver
SetupItem virtualMonitorItem()
{
	const bool present = WaitNamedPipeW(L"\\\\.\\pipe\\MTTVirtualDisplayPipe", 0) || GetLastError() == ERROR_SEM_TIMEOUT;
	return item("Virtual monitors (optional)", true,
	            present ? "The Virtual Display Driver is installed: Add monitor and Extra display create virtual monitors"
	                    : "Add monitor and Extra display use your physical monitors only. For virtual monitors, install the "
	                      "Virtual Display Driver: github.com/VirtualDrivers/Virtual-Display-Driver");
}
} // namespace

QList<SetupItem> runSetupChecks()
{
	return {encoderItem(), firewallItem(), audioItem(), libraryItem(), openxrItem(), openvrItem(), dxvkItem(), virtualMonitorItem()};
}

SetupDialog::SetupDialog(QWidget * parent) :
        QDialog(parent)
{
	setObjectName("vdDialog");
	setWindowTitle("OpenDesk Streamer - System check");
	setMinimumWidth(620);
	auto * v = new QVBoxLayout(this);

	auto * title = new QLabel("System check");
	title->setObjectName("platformName");
	auto * note = new QLabel("Checks what streaming to the headset needs on this system, and fixes what it can.");
	note->setObjectName("accountIds");
	note->setWordWrap(true);
	v->addWidget(title);
	v->addWidget(note);
	v->addSpacing(6);

	auto * host = new QWidget;
	list_ = new QVBoxLayout(host);
	list_->setContentsMargins(0, 0, 0, 0);
	v->addWidget(host);

	auto * startup = new QCheckBox("Check at startup");
	startup->setChecked(StreamerSettings::instance().checkSetupAtStartup);
	connect(startup, &QCheckBox::toggled, this, [](bool on) {
		StreamerSettings::instance().checkSetupAtStartup = on;
		StreamerSettings::instance().save();
	});

	auto * buttons = new QHBoxLayout;
	fixButton_ = new QPushButton("Fix automatically");
	fixButton_->setToolTip("Applies the fixes; Windows asks for administrator rights when one needs them");
	auto * close = new QPushButton("Close");
	connect(fixButton_, &QPushButton::clicked, this, &SetupDialog::fix);
	connect(close, &QPushButton::clicked, this, &QDialog::accept);
	buttons->addWidget(startup);
	buttons->addStretch();
	buttons->addWidget(fixButton_);
	buttons->addWidget(close);
	v->addLayout(buttons);
	refresh();
}

bool SetupDialog::hasProblems() const
{
	return std::any_of(items_.begin(), items_.end(), [](const SetupItem & i) { return !i.ok; });
}

void SetupDialog::refresh()
{
	while (QLayoutItem * i = list_->takeAt(0))
	{
		if (i->widget())
			i->widget()->deleteLater();
		delete i;
	}
	items_ = runSetupChecks();
	for (const SetupItem & i: items_)
	{
		auto * row = new QLabel(QString("%1  %2\n%3").arg(i.ok ? "[ok]" : "[!]", i.title, i.detail));
		row->setWordWrap(true);
		row->setObjectName(i.ok ? "accountId" : "statusLine");
		list_->addWidget(row);
	}
	fixButton_->setEnabled(std::any_of(items_.begin(), items_.end(), [](const SetupItem & i) { return !i.ok && i.userFix; }));
}

void SetupDialog::fix()
{
	QStringList errors;
	for (const SetupItem & i: items_)
		if (!i.ok && i.userFix)
		{
			const QString error = i.userFix();
			if (!error.isEmpty())
				errors << i.title + ": " + error;
		}
	refresh();
	if (!errors.isEmpty())
		QMessageBox::warning(this, windowTitle(), errors.join("\n\n"));
}
#endif
