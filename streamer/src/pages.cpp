#include "pages.h"
#include "engine.h"
#include "hardware_encoder.h"
#include "settings.h"
#include "setup.h"
#include "sysinfo.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QGridLayout>
#include <QJsonArray>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSlider>
#include <QUrl>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVBoxLayout>

#include <set>
#include <thread>

namespace
{
constexpr const char * notOnLinux = "\n(not implemented on Linux yet)";

QPixmap asset(const QString & name)
{
	return QPixmap(":/assets/opendesk/" + name);
}

// Tab content background: Grid Background="#101010" Margin="24 8 8 8"
QWidget * page(QWidget * self = nullptr)
{
	auto * w = self ? self : new QWidget;
	w->setObjectName("page");
	w->setAttribute(Qt::WA_StyledBackground);
	return w;
}

QLabel * section(const QString & text, int top = 0)
{
	auto * l = new QLabel(text);
	l->setObjectName("sectionLabel");
	l->setContentsMargins(4, top + 4, 0, 4);
	return l;
}

QCheckBox * check(const QString & text, bool & field, const QString & tooltip = {}, bool implemented = true,
                  std::function<void()> after = {})
{
	auto * c = new QCheckBox(text);
	c->setChecked(field);
	if (!tooltip.isEmpty() || !implemented)
		c->setToolTip(tooltip + (implemented ? "" : notOnLinux));
	QObject::connect(c, &QCheckBox::toggled, c, [&field, after](bool on) {
		field = on;
		StreamerSettings::instance().save();
		if (after)
			after();
	});
	QObject::connect(&StreamerSettings::instance(), &StreamerSettings::changed, c, [c, &field] {
		QSignalBlocker b(c);
		c->setChecked(field);
	});
	return c;
}

template <typename E>
QComboBox * combo(int width, const QList<std::pair<E, QString>> & items, E & field, std::function<void()> after = {})
{
	auto * c = new QComboBox;
	c->setFixedWidth(width);
	for (auto & [value, name]: items)
		c->addItem(name, int(value));
	auto sync = [c, &field] {
		QSignalBlocker b(c);
		c->setCurrentIndex(std::max(0, c->findData(int(field))));
	};
	sync();
	QObject::connect(c, &QComboBox::activated, c, [c, &field, after](int) {
		field = E(c->currentData().toInt());
		StreamerSettings::instance().save();
		if (after)
			after();
	});
	QObject::connect(&StreamerSettings::instance(), &StreamerSettings::changed, c, sync);
	return c;
}

// Slider + value text, as in the ADVANCED tab
// format shows field * display_scale (e.g. "%.0f%%" with 100 for percentages, like the Windows streamer)
QWidget * slider(double & field, double lo, double hi, double step, const QString & format, double display_scale = 1)
{
	auto * row = new QWidget;
	auto * h = new QHBoxLayout(row);
	h->setContentsMargins(4, 0, 0, 0);
	auto * s = new QSlider(Qt::Horizontal);
	s->setFixedWidth(180);
	s->setRange(0, int(std::round((hi - lo) / step)));
	auto * text = new QLabel;
	text->setMinimumWidth(60);
	auto sync = [=, &field] {
		QSignalBlocker b(s);
		s->setValue(int(std::round((field - lo) / step)));
		text->setText(QString::asprintf(qPrintable(format), field * display_scale));
	};
	sync();
	QObject::connect(s, &QSlider::valueChanged, s, [=, &field](int v) {
		field = lo + v * step;
		text->setText(QString::asprintf(qPrintable(format), field * display_scale));
	});
	QObject::connect(s, &QSlider::sliderReleased, s, [] { StreamerSettings::instance().save(); });
	QObject::connect(&StreamerSettings::instance(), &StreamerSettings::changed, s, sync);
	h->addWidget(s);
	h->addWidget(text);
	h->addStretch();
	return row;
}

QPushButton * resetButton(std::function<void()> fn)
{
	auto * b = new QPushButton("Reset to Defaults");
	b->setContentsMargins(20, 0, 20, 0);
	b->setStyleSheet("padding: 0 20px;");
	QObject::connect(b, &QPushButton::clicked, b, fn);
	return b;
}

// Removes and deletes everything in a layout, including widgets inside nested layouts
// (hidden first: deleteLater would otherwise leave them painted until the next event loop turn).
void clearLayout(QLayout * layout)
{
	while (QLayoutItem * item = layout->takeAt(0))
	{
		if (QWidget * w = item->widget())
		{
			w->hide();
			w->deleteLater();
		}
		else if (QLayout * child = item->layout())
			clearLayout(child);
		delete item;
	}
}

QScrollArea * scrollArea(QWidget * content)
{
	auto * sa = new QScrollArea;
	sa->setWidget(content);
	sa->setWidgetResizable(true);
	sa->setFrameShape(QFrame::NoFrame);
	sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	return sa;
}
} // namespace

// ---------------------------------------------------------------------------------------------- ACCOUNTS

AccountsPage::AccountsPage(Engine &) :
        editing_(!StreamerSettings::instance().hasValidAccountID())
{
	page(this);
	auto * outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto * content = page();
	list_ = new QVBoxLayout(content);
	list_->setContentsMargins(24, 8, 8, 8);
	outer->addWidget(scrollArea(content));
	rebuild();
}

void AccountsPage::addAccount(int platform, const QString & account)
{
	auto & s = StreamerSettings::instance();
	if (!s.accounts[platform].contains(account))
		s.accounts[platform] << account;
	s.save();
	rebuild();
}

void AccountsPage::rebuild()
{
	clearLayout(list_);
	auto & s = StreamerSettings::instance();

	for (const auto & p: platforms())
	{
		const QStringList ids = s.accounts.value(int(p.platform));
		if (ids.isEmpty())
			continue;
		auto * header = new QWidget;
		auto * hh = new QHBoxLayout(header);
		hh->setContentsMargins(4, 0, 0, 0);
		auto * icon = new QLabel;
		icon->setPixmap(asset(p.icon).scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation));
		auto * name = new QLabel(p.name);
		name->setObjectName("platformName");
		hh->addWidget(icon);
		hh->addWidget(name);
		hh->addStretch();
		list_->addWidget(header);

		if (!editing_)
		{
			auto * text = new QLabel(ids.join(", "));
			text->setObjectName("accountIds");
			text->setWordWrap(true);
			text->setContentsMargins(4, 0, 6, 6);
			list_->addWidget(text);
			continue;
		}
		for (const QString & id: ids)
		{
			auto * row = new QWidget;
			auto * rh = new QHBoxLayout(row);
			rh->setContentsMargins(4, 0, 0, 0);
			auto * label = new QLabel(id);
			label->setObjectName("aboutValue");
			auto * remove = new QPushButton;
			remove->setObjectName("imageButton");
			remove->setIcon(asset("clear20.png"));
			remove->setIconSize({20, 20});
			remove->setToolTip("Remove");
			connect(remove, &QPushButton::clicked, this, [this, platform = int(p.platform), id] {
				auto & st = StreamerSettings::instance();
				st.accounts[platform].removeAll(id);
				st.save();
				QMetaObject::invokeMethod(this, &AccountsPage::rebuild, Qt::QueuedConnection);
			});
			rh->addWidget(label);
			rh->addWidget(remove);
			rh->addStretch();
			list_->addWidget(row);
		}
	}

	if (editing_ && !s.hasValidAccountID())
	{
		auto * warn = new QLabel("You must add an account to make your computer accessible\n"
		                         "Follow the instructions on your device");
		warn->setObjectName("warning");
		warn->setWordWrap(true);
		warn->setContentsMargins(0, 0, 24, 18);
		list_->addWidget(warn);
	}

	if (editing_)
	{
		auto * add = new QPushButton("Add account ▾");
		add->setObjectName("linkButton");
		auto * menu = new QMenu(add);
		for (const auto & p: platforms())
		{
			menu->addAction(QIcon(asset(p.icon)), p.name, this, [this, p] {
				// Windows resolves a user name through upstream's servers; on Linux we take the AccountID directly
				// (shown by `sniff`, or accepted from an "Ask for computer access" prompt).
				QDialog dlg(this);
				dlg.setObjectName("vdDialog");
				dlg.setWindowTitle("Add Account");
				auto * v = new QVBoxLayout(&dlg);
				auto * title = new QLabel(QString("Add %1 account").arg(p.name));
				title->setObjectName("platformName");
				auto * hint = new QLabel("Enter the account ID of the headset.\n"
				                         "Tip: leave \"Ask for computer access\" enabled and the headset will\n"
				                         "prompt here when it searches for computers.");
				hint->setObjectName("accountIds");
				auto * edit = new QLineEdit;
				auto * buttons = new QDialogButtonBox;
				buttons->addButton("Add", QDialogButtonBox::AcceptRole);
				buttons->addButton("Cancel", QDialogButtonBox::RejectRole);
				connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
				connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
				v->addWidget(title);
				v->addWidget(hint);
				v->addWidget(edit);
				v->addWidget(buttons);
				if (dlg.exec() == QDialog::Accepted && !edit->text().trimmed().isEmpty())
					addAccount(int(p.platform), edit->text().trimmed());
			});
		}
		connect(add, &QPushButton::clicked, add, [add, menu] { menu->popup(add->mapToGlobal(QPoint(0, add->height()))); });
		auto * row = new QHBoxLayout;
		row->addWidget(add);
		row->addStretch();
		list_->addSpacing(10);
		list_->addLayout(row);
	}

	// Windows: "Change" (view mode) enters editing; "Save" (editing, needs an account) commits and leaves it
	auto * change = new QPushButton(editing_ ? "Save" : "Change");
	change->setVisible(!editing_ || s.hasValidAccountID());
	connect(change, &QPushButton::clicked, this, [this] {
		if (editing_)
			StreamerSettings::instance().save();
		editing_ = !editing_;
		QMetaObject::invokeMethod(this, &AccountsPage::rebuild, Qt::QueuedConnection);
	});
	auto * row = new QHBoxLayout;
	row->addStretch();
	row->addWidget(change);
	row->addStretch();
	list_->addSpacing(10);
	list_->addLayout(row);
	list_->addStretch();
}

// ---------------------------------------------------------------------------------------------- BINDINGS

QWidget * makeBindingsPage()
{
	static const QMap<QString, QString> names{
	        {"SwitchMonitor", "Switch monitor"},
	        {"ToggleVRMode", "Toggle VR mode"},
	        {"ToggleVRPassthrough", "Toggle VR passthrough"},
	        {"EnableVRPassthrough", "Enable VR passthrough"},
	        {"DisableVRPassthrough", "Disable VR passthrough"},
	        {"ToggleHandPassthrough", "Toggle hand passthrough"},
	        {"ToggleDeskPassthrough", "Toggle desk passthrough"},
	        {"TogglePerformanceOverlay", "Toggle performance overlay"},
	        {"ToggleFoveatedStreaming", "Toggle foveated streaming"},
	};
	auto * w = page();
	auto * v = new QVBoxLayout(w);
	v->setContentsMargins(24, 8, 8, 8);
	v->addWidget(section("Keyboard shortcuts"));
	auto * grid_host = page();
	auto * grid = new QGridLayout(grid_host);
	grid->setContentsMargins(4, 0, 8, 0);
	int row = 0;
	for (const QString & action: bindingActions)
	{
		auto * label = new QLabel(names.value(action, action));
		label->setObjectName("aboutValue");
		auto * edit = new QKeySequenceEdit(QKeySequence(StreamerSettings::instance().bindings.value(action)));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		edit->setMaximumSequenceLength(1);
#endif
		edit->setFixedWidth(200);
		edit->setToolTip(QString("Global shortcut for \"%1\" (registered with the desktop; KDE: System Settings > Shortcuts)")
		                         .arg(names.value(action)));
		QObject::connect(edit, &QKeySequenceEdit::editingFinished, edit, [edit, action] {
			if (edit->keySequence().count() > 1)
				edit->setKeySequence(QKeySequence(edit->keySequence()[0]));
			StreamerSettings::instance().bindings[action] = edit->keySequence().toString();
			StreamerSettings::instance().save();
		});
		QObject::connect(&StreamerSettings::instance(), &StreamerSettings::changed, edit, [edit, action] {
			QSignalBlocker b(edit);
			edit->setKeySequence(QKeySequence(StreamerSettings::instance().bindings.value(action)));
		});
		grid->addWidget(label, row, 0);
		grid->addWidget(edit, row++, 1, Qt::AlignLeft);
	}
	grid->setColumnStretch(2, 1);
	grid->setRowStretch(row, 1);
	v->addWidget(scrollArea(grid_host), 1);
	auto * reset = resetButton([] { StreamerSettings::instance().resetBindings(); });
	v->addWidget(reset, 0, Qt::AlignRight);
	return w;
}

// ---------------------------------------------------------------------------------------------- OPTIONS

// "Start with login": XDG autostart entry, written/removed only when the user toggles the option
void setAutostart(bool on)
{
	QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath("autostart/opendesk-streamer.desktop");
	if (!on)
	{
		QFile::remove(path);
		return;
	}
	QDir().mkpath(QFileInfo(path).path());
	QFile f(path);
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write(QString("[Desktop Entry]\nType=Application\nName=OpenDesk Streamer\nExec=%1\n"
		                "X-GNOME-Autostart-enabled=true\n").arg(QCoreApplication::applicationFilePath()).toUtf8());
}

// Preferred Codec: only what the GPU encodes (probed in the background: a few encoder opens). A codec chosen before that
// the GPU can't do goes back to Automatic. H.264+ is H.264's VR multi-stream mode.
void hideUnsupportedCodecs(QComboBox * box, std::vector<std::string> nodes, std::function<void()> after)
{
	QPointer<QComboBox> guard(box);
	std::thread([guard, nodes, after] {
		std::set<int> ok{int(VideoCodec::Automatic)};
		for (size_t i = 0; i < nodes.size(); ++i)
		{
			std::set<int> here;
			for (od::video_codec c: od::hardware_encoder_codecs(nodes[i]))
				here.insert(int(c));
			if (here.contains(int(VideoCodec::H264)))
				here.insert(int(VideoCodec::H264Plus));
			if (i == 0)
				ok.insert(here.begin(), here.end());
			else // both GPUs encode: the desktop's must do it too
				std::erase_if(ok, [&](int c) { return c != int(VideoCodec::Automatic) && !here.contains(c); });
		}
		QMetaObject::invokeMethod(qApp, [guard, ok, after] {
			if (!guard)
				return;
			auto * view = qobject_cast<QListView *>(guard->view());
			for (int row = 0; row < guard->count(); ++row)
				if (view)
					view->setRowHidden(row, !ok.contains(guard->itemData(row).toInt()));
			auto & s = StreamerSettings::instance();
			if (!ok.contains(int(s.preferredCodec)))
			{
				s.preferredCodec = VideoCodec::Automatic;
				s.save();
				guard->setCurrentIndex(guard->findData(int(VideoCodec::Automatic)));
				if (after)
					after();
			}
		});
	}).detach();
}

QWidget * makeOptionsPage(Engine & engine)
{
	auto & s = StreamerSettings::instance();
	auto restart = [&engine] { engine.applyOptions(); };
	auto apply = restart;

	auto * w = page();
	auto * grid = new QGridLayout(w);
	grid->setContentsMargins(24, 8, 8, 8);
	grid->setHorizontalSpacing(0);
	grid->setColumnMinimumWidth(0, 246);

	auto * left = new QVBoxLayout;
	left->setSpacing(2);
	// encoder options depend on the GPU(s) encoding: the desktop's and VR's (the same one except on hybrid laptops)
	const std::string desktop_node = od::default_render_node(), vr_node = od::vr_render_node();
	std::vector<std::string> nodes{vr_node};
	if (desktop_node != vr_node)
		nodes.push_back(desktop_node);
	bool nvidia = false, amd = false;
	for (const auto & n: nodes)
	{
		nvidia |= od::gpu_vendor(n) == 0x10de;
		amd |= od::gpu_vendor(n) == 0x1002;
	}
	left->addWidget(section("Preferred Codec"));
	auto * codecs = combo<VideoCodec>(180,
	                                  {{VideoCodec::Automatic, "Automatic"},
	                                   {VideoCodec::H264, "H.264"},
	                                   {VideoCodec::H264Plus, "H.264+"},
	                                   {VideoCodec::HEVC, "HEVC"},
	                                   {VideoCodec::HEVC10bit, "HEVC 10-bit"},
	                                   {VideoCodec::AV1, "AV1"},
	                                   {VideoCodec::AV110bit, "AV1 10-bit"}},
	                                  s.preferredCodec, restart);
	left->addWidget(codecs, 0, Qt::AlignLeft);
	hideUnsupportedCodecs(codecs, nodes, restart);
	if (amd || nvidia) // VAAPI on Intel has neither
	{
		left->addWidget(check("Adaptive quantization", s.enableAQ,
		                      QString("Reduces compression artifacts in dark scenes (recommended)\n") +
		                              (nvidia ? "NVENC spatial adaptive quantization" : "AMD VBAQ (variance-based adaptive quantization)"),
		                      true, apply));
		left->addWidget(check("2-Pass encoding", s.enable2Pass,
		                      QString("Improves compression quality but uses some GPU horsepower to do so\n"
		                              "Only use when you have performance headroom to spare\n") +
		                              (nvidia ? "NVENC quarter-resolution two-pass rate control" : "AMD pre-encode analysis"),
		                      true, apply));
	}
	if (nvidia) // AMD/Intel already encode with their fastest preset
		left->addWidget(check("Adaptive encoder speed", s.adaptiveEncoderSpeed,
		                      "VR: when encoding can't keep up with the headset's refresh rate, switch NVENC to faster presets\n"
		                      "(slightly less detail per bit, full frame rate) and back once there is headroom again",
		                      true, apply));
	left->addWidget(section("Gamepad Emulation", 8));
	left->addWidget(combo<GamepadEmulation>(180,
	                                        {{GamepadEmulation::Automatic, "Automatic"},
	                                         {GamepadEmulation::Xbox, "Xbox"},
	                                         {GamepadEmulation::Dualshock, "Dualshock"}},
	                                        s.gamepadEmulation),
	                0, Qt::AlignLeft);
	left->addWidget(section("Audio Streaming", 8));
	left->addWidget(combo<AudioStreaming>(210,
	                                      {{AudioStreaming::Disabled, "Computer only"},
	                                       {AudioStreaming::HeadsetOnly, "VR headset only"},
	                                       {AudioStreaming::HeadsetAndComputer, "VR headset & computer"}},
	                                      s.audioStreaming, apply),
	                0, Qt::AlignLeft);
	left->addWidget(check("Use virtual audio driver", s.useVirtualAudioDriver,
	                      "Enable this option if you experience issues streaming audio\n"
	                      "Linux: 'VR headset only' routes PC audio to a 'OpenDesk Audio' sink while connected;\n"
	                      "off = the real output is muted instead",
	                      true, apply));
	left->addStretch();

	auto * right = new QVBoxLayout;
	right->setSpacing(0);
	right->addWidget(check("Encrypt local traffic", s.encryptLocalTraffic,
	                       "Also encrypts video, audio and input on your local network (applies to the next connection)", true,
	                       apply));
	right->addWidget(check("Automatically adjust bitrate", s.autoAdjustBitrate,
	                       "Adjusts the video bitrate based on the available bandwidth (requires re-connection)", true,
	                       [&engine] { engine.applyOptions(); }));
	right->addWidget(check("Start with login", s.startWithLogin, "Adds an XDG autostart entry (~/.config/autostart)", true,
	                       [] { setAutostart(StreamerSettings::instance().startWithLogin); }));
	right->addWidget(check("Start minimized in tray", s.startMinimizedInTray));
	right->addWidget(check("Use touch input", s.useTouchInput,
	                       "Enable to allow touch gestures. Disable if clicks don't work correctly", true,
	                       [&engine] { engine.applyOptions(); }));
	right->addWidget(check("Lock computer on disconnect", s.lockComputer, "Locks the session (loginctl lock-session) when the headset disconnects",
	                       true, apply));
	right->addWidget(check("Auto-select microphone", s.autoSelectMicrophone,
	                       "Automatically selects the OpenDesk Microphone when Microphone passthrough is "
	                       "enabled in VR",
	                       true, apply));
	right->addWidget(check("Ask for computer access", s.showPairingRequests,
	                       "Shows a prompt when a new headset is searching for computers in your local network"));
	right->addStretch();

	grid->addLayout(left, 0, 0, 2, 1);
	grid->addLayout(right, 0, 1);
	grid->setColumnStretch(1, 1);
	grid->setRowStretch(0, 1);
	grid->addWidget(resetButton([] { StreamerSettings::instance().resetOptions(); }), 1, 1,
	                Qt::AlignRight | Qt::AlignBottom);
	return w;
}

// ---------------------------------------------------------------------------------------------- ADVANCED

QWidget * makeAdvancedPage(Engine & engine)
{
	auto & s = StreamerSettings::instance();
	auto * w = page();
	auto * outer = new QVBoxLayout(w);
	outer->setContentsMargins(24, 8, 8, 8);
	// more settings than fit: they scroll, Reset to Defaults stays
	auto * content = page();
	auto * v = new QVBoxLayout(content);
	v->setContentsMargins(0, 0, 8, 0);
	v->setSpacing(2);

	auto * info = new QWidget;
	auto * ih = new QHBoxLayout(info);
	ih->setContentsMargins(4, 0, 0, 0);
	auto * icon = new QLabel;
	icon->setPixmap(asset("info.png"));
	auto * note = new QLabel("These settings apply to VR games using OpenDesk XR.");
	note->setObjectName("accountIds");
	ih->addWidget(icon);
	ih->addWidget(note);
	ih->addStretch();
	v->addWidget(info);

	v->addWidget(section("Reported Streamer Version (compatibility)", 10));
	auto * reported = new QLineEdit(s.reportedStreamerVersion);
	reported->setMaximumWidth(160);
	reported->setPlaceholderText("1.20.3");
	reported->setToolTip("The version this streamer advertises to the headset app.\n"
	                     "The headset refuses a streamer with a higher minor version, so 1.20.3 connects to every\n"
	                     "headset app from 1.20.3 onward. 1.34.23 unlocks eye-tracked foveated streaming and stream\n"
	                     "restart on current headsets. Takes effect on the next connection.");
	QObject::connect(reported, &QLineEdit::editingFinished, [&engine, reported] {
		const QString text = reported->text().trimmed();
		if (!QRegularExpression("^[0-9]+(\\.[0-9]+){0,3}$").match(text).hasMatch())
		{
			reported->setText(StreamerSettings::instance().reportedStreamerVersion);
			return;
		}
		StreamerSettings::instance().reportedStreamerVersion = text;
		StreamerSettings::instance().save();
		engine.applyOptions();
	});
	v->addWidget(reported);

	v->addWidget(section("Horizontal FOV Tangent", 4));
	v->addWidget(slider(s.horizontalFovTangent, 0.5, 1.0, 0.01, "%.2f"));
	v->addWidget(section("Vertical FOV Tangent", 10));
	v->addWidget(slider(s.verticalFovTangent, 0.5, 1.0, 0.01, "%.2f"));
	v->addWidget(section("OpenDesk XR Render Resolution", 10));
	v->addWidget(slider(s.renderResolutionOverride, 0.5, 2.0, 0.05, "%.2fx"));
	auto * fovea = section("Fovea Size", 10);
	fovea->setToolTip("Foveated streaming (eye-tracked headsets): the region around your gaze sent at full resolution, as a\n"
	                  "fraction of the eye image. Bigger = sharper area, more to encode and decode.\n"
	                  "Windows upstream: 15-35%, default 25%; up to 50% here (more for the headset to decode)");
	v->addWidget(fovea);
	v->addWidget(slider(s.foveaSize, 0.15, 0.5, 0.01, "%.0f%%", 100)); // Windows: 15-35%, default 25%; above that more to decode
	auto * periphery = section("Periphery Resolution", 10);
	periphery->setToolTip("Foveated streaming (eye-tracked headsets): resolution outside the fovea, as a fraction of the eye\n"
	                      "resolution (Windows: always 33% - its encode size halved). Lower = less to encode and decode - helps headsets with weaker\n"
	                      "decoders (Quest Pro) at high quality / refresh rates; the fovea stays sharp");
	v->addWidget(periphery);
	v->addWidget(slider(s.peripheryScale, 0.3, 0.5, 0.01, "%.0f%%", 100));
	v->addWidget(check("Soft fovea edge", s.softFoveaEdge,
	                   "Foveated streaming: the sharp area loses detail gradually toward its edge and ends in the\n"
	                   "periphery's own pixels. Off: a hard edge, like the Windows streamer. Takes effect right away",
	                   true, [&engine] { engine.applyOptions(); }));
	v->addWidget(check("Additional sharpening", s.useSharpening,
	                   "Adds contrast-adaptive sharpening before video encoding.\n"
	                   "Doesn't require a restart of the game to take effect",
	                   true, [&engine] { engine.applyOptions(); }));
	v->addWidget(slider(s.sharpening, 0.0, 1.0, 0.05, "%.2f"));
	v->addWidget(check("Use Fov Stencil", s.useFovStencil,
	                   "Optimizes game rendering by not rendering the corners. Only uncheck when recording gameplay "
	                   "(requires restart of the game)",
	                   true, [&engine] { engine.applyOptions(); }));
	v->addWidget(check("Boost game priority", s.boostGamePriority,
	                   "Only enable if experiencing game freezes or stutter. Raises the priority of the game's render thread\n"
	                   "(Linux: CPU priority through rtkit; Windows raises the GPU priority, which needs root here).\n"
	                   "Takes effect right away",
	                   true, [&engine] { engine.applyOptions(); }));
	auto * mouth = section("Mouth Sensitivity", 10);
	mouth->setToolTip("Face tracking to VRChat (Quest Pro): how strongly the avatar's mouth and jaw follow yours. 1.0x = the\n"
	                  "headset's values as they are; higher makes normal talking visible without opening wide (closed\n"
	                  "stays closed, fully open stays fully open). Takes effect right away");
	v->addWidget(mouth);
	v->addWidget(slider(s.mouthSensitivity, 1.0, 4.0, 0.1, "%.1fx"));

	v->addStretch();
	outer->addWidget(scrollArea(content), 1);
	outer->addWidget(resetButton([] { StreamerSettings::instance().resetAdvanced(); }), 0, Qt::AlignRight);
	return w;
}

// ---------------------------------------------------------------------------------------------- ABOUT

AboutPage::AboutPage(Engine & engine) :
        engine_(engine)
{
	page(this);
	auto * grid = new QGridLayout(this);
	grid->setContentsMargins(24, 8, 8, 8);
	grid->setVerticalSpacing(2);
	int row = 0;
	auto add = [&](const QString & key, QWidget * value, const QString & tooltip = {}) {
		auto * k = new QLabel(key);
		k->setObjectName("aboutKey");
		k->setToolTip(tooltip);
		k->setAlignment(Qt::AlignRight | Qt::AlignTop);
		grid->addWidget(k, row, 0);
		grid->addWidget(value, row++, 1);
	};
	auto text = [](const QString & t) {
		auto * l = new QLabel(t);
		l->setObjectName("aboutValue");
		l->setTextInteractionFlags(Qt::TextSelectableByMouse);
		return l;
	};
	auto link = [](const QString & url) {
		auto * l = new QLabel(QString("<a href=\"%1\" style=\"color:#A58CFF\">%1</a>").arg(url));
		l->setObjectName("aboutValue");
		l->setOpenExternalLinks(true);
		return l;
	};

	headset_ = text("Not connected");
	codec_ = text("-");
	latency_ = text("-");
	ethernet_ = text("-");
	autoBitrate_ = text("-");

	add("Graphics Card:", text(sysinfo::graphicsCards()));
	add("Headset:", headset_);
	add("Codec:", codec_);
	add("Latency:", latency_);
	add("Processor:", text(sysinfo::processor()));
	add("Hardware Encoder:", text(sysinfo::hasHardwareEncoder() ? "Yes" : "No"),
	    "A hardware video encoder is required for multi-monitor or VR game streaming");
	add("Memory:", text(sysinfo::memory()));
	add("Operating System:", text(sysinfo::operatingSystem()));
	add("Secure Boot:", text(sysinfo::secureBoot()),
	    "When Secure Boot is disabled in BIOS, it can solve compatibility with some games");
	add("PC Ethernet:", ethernet_);
	add("Auto Adjust Bitrate:", autoBitrate_);
	add("Remote Routing:", text("Not supported: local network only"));
	QString router = sysinfo::routerUrl();
	add("Router Settings:", router.isEmpty() ? text("-") : link(router));
	// open-source projects this project builds on
	auto * credits = new QLabel(
	        "<a href=\"https://github.com/mbucchia/VirtualDesktop-OpenXR\" style=\"color:#A58CFF\">VDXR / OpenDesk XR</a>"
	        " by Matthieu Bucchianeri (MIT)<br>"
	        "<a href=\"https://github.com/Supreeeme/xrizer\" style=\"color:#A58CFF\">xrizer</a>"
	        " by Supreeeme et al. (GPL-3.0)<br>"
	        "<a href=\"https://github.com/benaclejames/VRCFaceTracking\" style=\"color:#A58CFF\">VRCFaceTracking</a>"
	        " by benaclejames et al. (Apache-2.0)");
	credits->setToolTip("OpenDesk XR is derived from VDXR by Matthieu Bucchianeri (MIT): an OpenXR runtime for OpenDesk.\n"
	                    "xrizer by Supreeeme and contributors: runs OpenVR games on that runtime; downloaded separately, not part of this program.\n"
	                    "VRCFaceTracking by benaclejames and contributors: its VRChat face tracking parameters, used by the built-in OSC output.");
	credits->setObjectName("aboutValue");
	credits->setOpenExternalLinks(true);
	add("Credits:", credits);
	add("Project:", text("Independent open-source PC streamer"));

	auto * buttons = new QHBoxLayout;
	auto * logs = new QPushButton("Show logs");
	connect(logs, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(engine_.logPath())); });
	auto * check = new QPushButton("Check setup...");
	connect(check, &QPushButton::clicked, this, [this] { SetupDialog(window()).exec(); });
	buttons->addWidget(check);
	buttons->addWidget(logs);
	buttons->addStretch();
	grid->addLayout(buttons, row++, 0, 1, 2);
	grid->setColumnStretch(1, 1);
	grid->setRowStretch(row, 1);

	connect(&engine, &Engine::headsetChanged, this, &AboutPage::refresh);
	connect(&engine, &Engine::videoChanged, this, &AboutPage::refresh);
	connect(&engine, &Engine::statusChanged, this, &AboutPage::refresh);
	connect(&engine, &Engine::ready, this, &AboutPage::refresh);
	connect(&StreamerSettings::instance(), &StreamerSettings::changed, this, &AboutPage::refresh);
	refresh();
}

void AboutPage::refresh()
{
	const auto & m = engine_.headsetMobileSettings;
	if (engine_.status() == Engine::Status::Connected && !m.isEmpty())
	{
		auto res = m["Resolution"].toObject();
		headset_->setText(QString("%1 (%2)").arg(hmdTypeName(m["HmdType"].toInt()),
		                                         engine_.connectedViaUsb ? "USB" : "Wi-Fi"));
	}
	else
		headset_->setText(engine_.status() == Engine::Status::Stopped ? "Streamer not running" : "Not connected");

	const auto & v = engine_.lastVideo;
	if (!engine_.captureErrorMessage.isEmpty())
		codec_->setText("Screen capture failed: " + engine_.captureErrorMessage);
	else
		codec_->setText(v.isEmpty() ? "-"
	                            : QString("%1 %2 fps, %3 Mbps (%4)")
	                                      .arg(v["codec"].toString())
	                                      .arg(v["fps"].toInt())
	                                      .arg(v["kbps"].toInt() / 1000)
	                                      .arg(v["mode"].toString()));
	latency_->setText("-");

	QStringList eth;
	for (const auto & a: engine_.computer["adapters"].toArray())
	{
		auto o = a.toObject();
		if (!o["wifi"].toBool() && !o["usb"].toBool())
			eth << QString("%1 (%2)").arg(o["ip"].toString(), o["gbit"].toBool() ? "1 Gbps+" : "< 1 Gbps");
	}
	ethernet_->setText(eth.isEmpty() ? "No" : "Yes - " + eth.join(", "));
	autoBitrate_->setText(StreamerSettings::instance().autoAdjustBitrate ? "Yes" : "No");
}
