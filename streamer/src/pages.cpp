#include "pages.h"
#include "engine.h"
#include "hardware_encoder.h"
#include "settings.h"
#include "setup.h"
#include "sysinfo.h"
#ifdef _WIN32
#include "dxvk_win.h"
#include "games.h"
#include <QListWidget>
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFrame>
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
constexpr const char * notOnLinux = "\n(not implemented yet)";

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

// A settings tab: cards of rows that scroll, "Reset to Defaults" below them
struct SettingsPage
{
	QWidget * widget;
	QVBoxLayout * cards;
};

SettingsPage settingsPage(std::function<void()> reset, const QString & note = {})
{
	auto * w = page();
	auto * outer = new QVBoxLayout(w);
	outer->setContentsMargins(16, 12, 8, 10);
	outer->setSpacing(8);
	auto * content = page();
	auto * v = new QVBoxLayout(content);
	v->setContentsMargins(0, 0, 8, 0);
	v->setSpacing(12);
	if (!note.isEmpty())
	{
		auto * info = new QWidget;
		auto * ih = new QHBoxLayout(info);
		ih->setContentsMargins(2, 0, 0, 0);
		auto * icon = new QLabel;
		icon->setPixmap(asset("info.png").scaled(18, 18, Qt::KeepAspectRatio, Qt::SmoothTransformation));
		auto * text = new QLabel(note);
		text->setObjectName("pageNote");
		text->setWordWrap(true);
		ih->addWidget(icon);
		ih->addWidget(text, 1);
		v->addWidget(info);
	}
	outer->addWidget(scrollArea(content), 1);
	if (reset)
		outer->addWidget(resetButton(reset), 0, Qt::AlignRight);
	return {w, v};
}

// A titled group of settings
QVBoxLayout * card(QVBoxLayout * cards, const QString & title)
{
	auto * frame = new QFrame;
	frame->setObjectName("card");
	auto * v = new QVBoxLayout(frame);
	v->setContentsMargins(16, 12, 16, 8);
	v->setSpacing(0);
	auto * t = new QLabel(title.toUpper());
	t->setObjectName("cardTitle");
	t->setContentsMargins(0, 0, 0, 4);
	v->addWidget(t);
	cards->addWidget(frame);
	return v;
}

QVBoxLayout * card(SettingsPage & p, const QString & title)
{
	return card(p.cards, title);
}

// an icon from the assets, sharp at the screen's pixel ratio
QPixmap icon(const QString & name, int size)
{
	return QIcon(":/assets/opendesk/" + name).pixmap(QSize(size, size), qApp->devicePixelRatio());
}

// One setting: name and a short description on the left, its control on the right. The long explanation is the
// row's tooltip.
QWidget * row(QVBoxLayout * card, const QString & title, const QString & description, QWidget * control,
              const QString & tooltip = {})
{
	auto * r = new QWidget;
	r->setObjectName("settingRow");
	r->setAttribute(Qt::WA_StyledBackground);
	auto * g = new QGridLayout(r);
	g->setContentsMargins(0, 8, 0, 8);
	g->setHorizontalSpacing(16);
	g->setVerticalSpacing(1);
	auto * t = new QLabel(title);
	t->setObjectName("settingTitle");
	g->addWidget(t, 0, 0);
	if (!description.isEmpty())
	{
		auto * d = new QLabel(description);
		d->setObjectName("settingDescription");
		d->setWordWrap(true);
		g->addWidget(d, 1, 0);
	}
	if (control)
		g->addWidget(control, 0, 1, description.isEmpty() ? 1 : 2, 1, Qt::AlignRight | Qt::AlignVCenter);
	g->setColumnStretch(0, 1);
	if (!tooltip.isEmpty())
		for (QWidget * w: r->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
			w->setToolTip(tooltip);
	if (card->count() > 1) // a hairline between rows
	{
		auto * line = new QFrame;
		line->setObjectName("rowSeparator");
		line->setFixedHeight(1);
		card->addWidget(line);
	}
	card->addWidget(r);
	return r;
}

// A switch (styled check box) bound to a setting
QCheckBox * toggle(bool & field, std::function<void()> after = {})
{
	auto * c = check({}, field, {}, true, std::move(after));
	c->setObjectName("switch");
	c->setCursor(Qt::PointingHandCursor);
	return c;
}

QWidget * toggleRow(QVBoxLayout * card, const QString & title, const QString & description, bool & field,
                    std::function<void()> after = {}, const QString & tooltip = {})
{
	return row(card, title, description, toggle(field, std::move(after)), tooltip);
}
} // namespace

// ---------------------------------------------------------------------------------------------- ACCOUNTS

AccountsPage::AccountsPage(Engine &)
{
	page(this);
	auto * outer = new QVBoxLayout(this);
	outer->setContentsMargins(16, 12, 8, 10);
	auto * content = page();
	list_ = new QVBoxLayout(content);
	list_->setContentsMargins(0, 0, 8, 0);
	list_->setSpacing(12);
	outer->addWidget(scrollArea(content), 1);
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

	if (!s.hasValidAccountID())
	{
		auto * c = card(list_, "Get started");
		auto * warn = new QLabel("Add the account of your headset to make this computer accessible.\n"
		                         "Open OpenDesk on the headset and look for this computer: with \"Ask for computer access\" on\n"
		                         "(General tab) you are asked here, or add the account ID yourself below.");
		warn->setObjectName("warning");
		warn->setWordWrap(true);
		warn->setContentsMargins(0, 4, 0, 8);
		c->addWidget(warn);
	}

	auto * c = card(list_, "Linked accounts");
	for (const auto & p: platforms())
		for (const QString & id: s.accounts.value(int(p.platform)))
		{
			if (c->count() > 1)
			{
				auto * line = new QFrame;
				line->setObjectName("rowSeparator");
				line->setFixedHeight(1);
				c->addWidget(line);
			}
			auto * r = new QWidget;
			auto * h = new QHBoxLayout(r);
			h->setContentsMargins(0, 8, 0, 8);
			h->setSpacing(12);
			auto * badge = new QLabel;
			badge->setPixmap(icon(p.icon, 28));
			auto * text = new QVBoxLayout;
			text->setSpacing(1);
			auto * name = new QLabel(id);
			name->setObjectName("settingTitle");
			name->setTextInteractionFlags(Qt::TextSelectableByMouse);
			auto * platform = new QLabel(p.name);
			platform->setObjectName("settingDescription");
			text->addWidget(name);
			text->addWidget(platform);
			auto * remove = new QPushButton("Remove");
			remove->setObjectName("dangerButton");
			connect(remove, &QPushButton::clicked, this, [this, platform = int(p.platform), id] {
				auto & st = StreamerSettings::instance();
				st.accounts[platform].removeAll(id);
				st.save();
				QMetaObject::invokeMethod(this, &AccountsPage::rebuild, Qt::QueuedConnection);
			});
			h->addWidget(badge);
			h->addLayout(text, 1);
			h->addWidget(remove);
			c->addWidget(r);
		}
	if (c->count() == 1)
	{
		auto * none = new QLabel("No accounts yet");
		none->setObjectName("settingDescription");
		none->setContentsMargins(0, 6, 0, 6);
		c->addWidget(none);
	}

	auto * add = new QPushButton("Add account");
	auto * menu = new QMenu(add);
	for (const auto & p: platforms())
	{
		menu->addAction(QIcon(QString(":/assets/opendesk/") + p.icon), p.name, this, [this, p] {
			QDialog dlg(this);
			dlg.setObjectName("vdDialog");
			dlg.setWindowTitle("Add Account");
			auto * v = new QVBoxLayout(&dlg);
			v->setContentsMargins(20, 16, 20, 16);
			v->setSpacing(10);
			auto * title = new QLabel(QString("Add %1 account").arg(p.name));
			title->setObjectName("platformName");
			auto * hint = new QLabel("Enter the account ID of the headset.\n"
			                         "Tip: leave \"Ask for computer access\" on and the headset will\n"
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
	auto * buttons = new QHBoxLayout;
	buttons->setContentsMargins(0, 8, 0, 6);
	buttons->addWidget(add);
	buttons->addStretch();
	c->addLayout(buttons);
	list_->addStretch();
}

// ---------------------------------------------------------------------------------------------- SHORTCUTS

QWidget * makeShortcutsPage()
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
	auto p = settingsPage([] { StreamerSettings::instance().resetBindings(); },
	                      "Keyboard shortcuts that act on the headset while it is connected. Click a field and press the keys.");
	auto * c = card(p, "Keyboard shortcuts");
	for (const QString & action: bindingActions)
	{
		auto * edit = new QKeySequenceEdit(QKeySequence(StreamerSettings::instance().bindings.value(action)));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		edit->setMaximumSequenceLength(1);
#endif
		edit->setFixedWidth(200);
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
		row(c, names.value(action, action), {}, edit, QString("Global shortcut for \"%1\"").arg(names.value(action)));
	}
	p.cards->addStretch();
	return p.widget;
}

// ---------------------------------------------------------------------------------------------- shared

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

// ---------------------------------------------------------------------------------------------- STREAMING

QWidget * makeStreamingPage(Engine & engine)
{
	auto & s = StreamerSettings::instance();
	auto apply = [&engine] { engine.applyOptions(); };
	auto p = settingsPage([] { StreamerSettings::instance().resetStreaming(); });

	// encoder options depend on the GPU(s) encoding: the desktop's and VR's (the same one except on hybrid laptops)
	const std::string desktop_node = od::desktop_encode_node(), vr_node = od::vr_encode_node();
	std::vector<std::string> nodes{vr_node};
	if (desktop_node != vr_node)
		nodes.push_back(desktop_node);
	bool nvidia = false, amd = false;
	for (const auto & n: nodes)
	{
		nvidia |= od::gpu_vendor(n) == 0x10de;
		amd |= od::gpu_vendor(n) == 0x1002;
	}
	const auto gpus = od::list_gpus();
	for (const auto & g: gpus) // (with two cards, either can become the encoder)
	{
		nvidia |= g.vendor == 0x10de;
		amd |= g.vendor == 0x1002;
	}

	auto * video = card(p, "Video encoding");
	auto * codecs = combo<VideoCodec>(170,
	                                  {{VideoCodec::Automatic, "Automatic"},
	                                   {VideoCodec::H264, "H.264"},
	                                   {VideoCodec::H264Plus, "H.264+"},
	                                   {VideoCodec::HEVC, "HEVC"},
	                                   {VideoCodec::HEVC10bit, "HEVC 10-bit"},
	                                   {VideoCodec::AV1, "AV1"},
	                                   {VideoCodec::AV110bit, "AV1 10-bit"}},
	                                  s.preferredCodec, apply);
	hideUnsupportedCodecs(codecs, nodes, apply);
	row(video, "Preferred codec", "Only codecs your GPU can encode are listed", codecs,
	    "H.264+ streams VR as several H.264 slices. 10-bit reduces banding in dark gradients.");
	if (amd || nvidia) // VAAPI on Intel has neither
	{
		toggleRow(video, "Adaptive quantization", "Fewer compression artifacts in dark scenes (recommended)", s.enableAQ, apply,
		          nvidia ? "NVENC spatial adaptive quantization" : "AMD VBAQ (variance-based adaptive quantization)");
		toggleRow(video, "2-pass encoding", "Better quality per bit, costs some GPU time", s.enable2Pass, apply,
		          QString("Only use when you have performance headroom to spare\n") +
		                  (nvidia ? "NVENC quarter-resolution two-pass rate control" : "AMD pre-encode analysis"));
	}
	if (nvidia) // AMD/Intel already encode with their fastest preset
	{
		row(video, "NVIDIA encoder quality", "Higher quality takes the encoder longer per frame",
		    combo<int>(170, {{1, "Fastest"}, {2, "Fast"}, {4, "Balanced"}, {6, "Best quality"}}, s.nvencQuality, apply),
		    "NVENC preset P1, P2, P4 or P6. With Adaptive encoder speed, VR falls back to faster presets when this one can't\n"
		    "keep up with the headset's refresh rate");
		toggleRow(video, "Adaptive encoder speed", "Keeps the full frame rate in VR when encoding falls behind",
		          s.adaptiveEncoderSpeed, apply,
		          "Switches NVENC to faster presets while encoding can't keep up with the headset's refresh rate\n"
		          "(slightly less detail per bit) and back once there is headroom again");
	}

	if (gpus.size() >= 2)
	{
		// sensible first choice: games where they run now, the stream on another card (an NVIDIA one if there is one)
		if (s.gameGpu.isEmpty() || s.streamingGpu.isEmpty())
		{
			const std::string render = od::vr_render_node();
			for (const auto & g: gpus)
				if (g.node == render && s.gameGpu.isEmpty())
					s.gameGpu = QString::fromStdString(g.pci);
			for (const auto & g: gpus)
				if (QString::fromStdString(g.pci) != s.gameGpu && (s.streamingGpu.isEmpty() || g.vendor == 0x10de))
					s.streamingGpu = QString::fromStdString(g.pci);
		}
		auto * cards = card(p, "Graphics cards");
		auto picker = [&gpus](QString & field, std::function<void()> after) {
			auto * c = new QComboBox;
			c->setFixedWidth(260);
			for (const auto & g: gpus)
				c->addItem(QString::fromStdString(g.name), QString::fromStdString(g.pci));
			auto sync = [c, &field] {
				QSignalBlocker b(c);
				c->setCurrentIndex(std::max(0, c->findData(field)));
			};
			sync();
			QObject::connect(c, &QComboBox::activated, c, [c, &field, after] {
				field = c->currentData().toString();
				StreamerSettings::instance().save();
				after();
			});
			QObject::connect(&StreamerSettings::instance(), &StreamerSettings::changed, c, sync);
			return c;
		};
		// the encoders of the newly chosen cards decide which codecs are offered
		auto gpus_changed = [&engine, codecs, apply] {
			engine.applyGpuSelection();
			std::vector<std::string> now{od::vr_encode_node()};
			if (const std::string d = od::desktop_encode_node(); d != now[0])
				now.push_back(d);
			hideUnsupportedCodecs(codecs, now, apply);
			apply();
		};
		toggleRow(cards, "Use two graphics cards", "One card runs the games, the other encodes the stream", s.dualGpu, gpus_changed,
		          "The game keeps all of its card, frames are copied to the streaming card over PCIe.\n"
		          "That copy needs a fast slot for the streaming card. Close VR games before changing this.");
		auto * game = row(cards, "Game card", "VR games render here", picker(s.gameGpu, gpus_changed));
		auto * streaming = row(cards, "Streaming card", "Encodes the video sent to the headset", picker(s.streamingGpu, gpus_changed));
		auto sync_enabled = [game, streaming] {
			const bool on = StreamerSettings::instance().dualGpu;
			game->setEnabled(on);
			streaming->setEnabled(on);
		};
		sync_enabled();
		QObject::connect(&StreamerSettings::instance(), &StreamerSettings::changed, game, sync_enabled);
	}

	auto * net = card(p, "Network");
	toggleRow(net, "Automatically adjust bitrate", "Follows the bandwidth the headset measures", s.autoAdjustBitrate, apply,
	          "Takes effect on the next connection");
	toggleRow(net, "Encrypt local traffic", "Also encrypt video, audio and input on your local network", s.encryptLocalTraffic,
	          apply, "Takes effect on the next connection");

	auto * compat = card(p, "Headset compatibility");
	auto * reported = new QLineEdit(s.reportedStreamerVersion);
	toggleRow(compat, "Match the headset app's version", "Reports the version the headset app expects (recommended)",
	          s.reportedVersionAuto,
	          [&engine, reported] {
		          reported->setEnabled(!StreamerSettings::instance().reportedVersionAuto);
		          engine.applyOptions();
	          },
	          "Headset apps from 1.34.25 tell the streamer their version: it is reported back, so the headset neither\n"
	          "refuses the PC nor asks to update it, and gets every feature it supports. Older headset apps get the\n"
	          "version below. Off: always report the version below");
	reported->setEnabled(!s.reportedVersionAuto);
	reported->setFixedWidth(120);
	reported->setPlaceholderText("1.20.3");
	QObject::connect(reported, &QLineEdit::editingFinished, reported, [&engine, reported] {
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
	QObject::connect(&StreamerSettings::instance(), &StreamerSettings::changed, reported, [reported] {
		auto & st = StreamerSettings::instance();
		reported->setText(st.reportedStreamerVersion);
		reported->setEnabled(!st.reportedVersionAuto);
	});
	row(compat, "Reported version", "For older headset apps, or all of them when matching is off", reported,
	    "The headset refuses a streamer with a higher minor version, so 1.20.3 connects to every headset app from\n"
	    "1.20.3 onward. 1.34.23 unlocks eye-tracked foveated streaming and stream restart on current headsets.\n"
	    "Takes effect on the next connection.");
	p.cards->addStretch();
	return p.widget;
}

// ---------------------------------------------------------------------------------------------- VR

QWidget * makeVRPage(Engine & engine)
{
	auto & s = StreamerSettings::instance();
	auto apply = [&engine] { engine.applyOptions(); };
	auto p = settingsPage([] { StreamerSettings::instance().resetVR(); }, "These settings apply to VR games running on OpenDesk XR.");

	auto * image = card(p, "Image");
	row(image, "Render resolution", "Resolution games render at, relative to the headset's",
	    slider(s.renderResolutionOverride, 0.5, 2.0, 0.05, "%.2fx"));
	row(image, "Horizontal FOV", "Narrower renders and streams less", slider(s.horizontalFovTangent, 0.5, 1.0, 0.01, "%.2f"),
	    "Tangent of the horizontal field of view, relative to the headset's");
	row(image, "Vertical FOV", "Narrower renders and streams less", slider(s.verticalFovTangent, 0.5, 1.0, 0.01, "%.2f"),
	    "Tangent of the vertical field of view, relative to the headset's");
	toggleRow(image, "FOV stencil", "Games skip the corners the lenses can't show", s.useFovStencil, apply,
	          "Saves GPU time with no visible difference in the headset. Turn off when recording gameplay\n"
	          "(requires restarting the game)");
	toggleRow(image, "Additional sharpening", "Contrast-adaptive sharpening before encoding", s.useSharpening, apply,
	          "Takes effect right away");
	auto * strength = row(image, "Sharpening strength", {}, slider(s.sharpening, 0.0, 1.0, 0.05, "%.2f"));
	strength->setEnabled(s.useSharpening); // only matters with sharpening on
	QObject::connect(&StreamerSettings::instance(), &StreamerSettings::changed, strength,
	                 [strength] { strength->setEnabled(StreamerSettings::instance().useSharpening); });

	auto * fov = card(p, "Foveated streaming (eye-tracked headsets)");
	row(fov, "Fovea size", "The area around your gaze sent at full resolution", slider(s.foveaSize, 0.15, 0.5, 0.01, "%.0f%%", 100),
	    "Fraction of the eye image. Bigger = sharper area, more to encode and decode.\n"
	    "15-35% is the usual range (25% default); up to 50% means more for the headset to decode");
	row(fov, "Periphery resolution", "Resolution outside the fovea", slider(s.peripheryScale, 0.3, 0.5, 0.01, "%.0f%%", 100),
	    "Fraction of the eye resolution. Lower = less to encode and decode - helps headsets with weaker\n"
	    "decoders (Quest Pro) at high quality / refresh rates; the fovea stays sharp");
	toggleRow(fov, "Soft fovea edge", "Blend the sharp area into the periphery gradually", s.softFoveaEdge, apply,
	          "Off: a hard edge between the sharp area and the periphery. Takes effect right away");

	auto * perf = card(p, "Performance");
#ifdef _WIN32
	toggleRow(perf, "Boost game priority", "Only if games freeze or stutter", s.boostGamePriority, apply,
	          "Raises the priority of the game's render thread (highest thread priority, process above normal).\n"
	          "Takes effect right away");

	// DXVK per game: Direct3D 11 games rendered through Vulkan
	auto * dxvk_games = new QListWidget;
	dxvk_games->setMinimumHeight(140);
	dxvk_games->setToolTip("Checked games run with DXVK: OpenDesk places DXVK's d3d11.dll and dxgi.dll next to the game and removes\n"
	                       "them again when unchecked. Games with anti-cheat may refuse modified files - leave those unchecked.\n"
	                       "Takes effect the next time the game starts");
	auto fill_dxvk = [dxvk_games](std::vector<od::game_entry> games) {
		QSignalBlocker block(dxvk_games);
		dxvk_games->clear();
		auto & st = StreamerSettings::instance();
		for (const auto & g: games)
		{
			if (g.install_dir.isEmpty())
				continue;
			auto * item = new QListWidgetItem(g.name, dxvk_games);
			item->setData(Qt::UserRole, od::dxvk::game_key(g));
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(od::dxvk::wanted(g, st.dxvkByDefault, st.dxvkGames) ? Qt::Checked : Qt::Unchecked);
		}
		if (games.empty())
			new QListWidgetItem("No VR games found", dxvk_games);
	};
	QObject::connect(dxvk_games, &QListWidget::itemChanged, dxvk_games, [&engine](QListWidgetItem * item) {
		const QString key = item->data(Qt::UserRole).toString();
		if (key.isEmpty())
			return;
		auto & st = StreamerSettings::instance();
		st.dxvkGames[key] = item->checkState() == Qt::Checked;
		st.save();
		engine.applyOptions();
	});
	toggleRow(perf, "DXVK by default", "Direct3D 11 games render through Vulkan unless set below", s.dxvkByDefault,
	          [&engine, dxvk_games] {
		          auto & st = StreamerSettings::instance();
		          for (int i = 0; i < dxvk_games->count(); ++i) // games without their own setting follow
		          {
			          auto * item = dxvk_games->item(i);
			          const QString key = item->data(Qt::UserRole).toString();
			          if (!key.isEmpty() && !st.dxvkGames.contains(key))
			          {
				          QSignalBlocker block(dxvk_games);
				          item->setCheckState(st.dxvkByDefault ? Qt::Checked : Qt::Unchecked);
			          }
		          }
		          engine.applyOptions();
	          });
	perf->addWidget(dxvk_games);
	{
		QPointer<QListWidget> list(dxvk_games);
		std::thread([list, fill_dxvk] {
			auto games = od::scan_games(); // reads the Steam / Oculus libraries: off the UI thread
			QMetaObject::invokeMethod(qApp, [list, fill_dxvk, games = std::move(games)]() mutable {
				if (list)
					fill_dxvk(std::move(games));
			});
		}).detach();
	}
#else
	toggleRow(perf, "Boost game priority", "Only if games freeze or stutter", s.boostGamePriority, apply,
	          "Raises the CPU priority of the game's render thread (through rtkit). Takes effect right away");
#endif

	auto * face = card(p, "Face tracking");
	row(face, "Mouth sensitivity", "How strongly the avatar's mouth follows yours (VRChat)",
	    slider(s.mouthSensitivity, 1.0, 4.0, 0.1, "%.1fx"),
	    "Face tracking to VRChat (Quest Pro): 1.0x = the headset's values as they are; higher makes normal\n"
	    "talking visible without opening wide (closed stays closed, fully open stays fully open). Takes effect right away");
	p.cards->addStretch();
	return p.widget;
}

// ---------------------------------------------------------------------------------------------- AUDIO & INPUT

QWidget * makeAudioInputPage(Engine & engine)
{
	auto & s = StreamerSettings::instance();
	auto apply = [&engine] { engine.applyOptions(); };
	auto p = settingsPage([] { StreamerSettings::instance().resetAudioInput(); });

	auto * audio = card(p, "Audio");
	row(audio, "Audio streaming", "Where the PC's sound plays while connected",
	    combo<AudioStreaming>(200,
	                          {{AudioStreaming::Disabled, "Computer only"},
	                           {AudioStreaming::HeadsetOnly, "VR headset only"},
	                           {AudioStreaming::HeadsetAndComputer, "VR headset & computer"}},
	                          s.audioStreaming, apply));
#ifndef _WIN32 // Windows: 'VR headset only' always mutes the PC's output (no virtual device without a driver)
	toggleRow(audio, "Use virtual audio device", "With 'VR headset only', PC sound goes to an 'OpenDesk Audio' device",
	          s.useVirtualAudioDriver, apply,
	          "Enable this option if you experience issues streaming audio. Off: the real output is muted instead");
#endif
	toggleRow(audio, "Select the headset microphone", "Makes it the default input while microphone passthrough is on",
	          s.autoSelectMicrophone, apply);

	auto * input = card(p, "Input");
	row(input, "Gamepad emulation", "The controller type games see",
	    combo<GamepadEmulation>(200,
	                            {{GamepadEmulation::Automatic, "Automatic"},
	                             {GamepadEmulation::Xbox, "Xbox"},
	                             {GamepadEmulation::Dualshock, "Dualshock"}},
	                            s.gamepadEmulation));
	toggleRow(input, "Touch input", "Allows touch gestures on the desktop", s.useTouchInput, apply,
	          "Disable if clicks don't work correctly");
	p.cards->addStretch();
	return p.widget;
}

// ---------------------------------------------------------------------------------------------- GENERAL

QWidget * makeGeneralPage(Engine & engine)
{
	auto & s = StreamerSettings::instance();
	auto apply = [&engine] { engine.applyOptions(); };
	auto p = settingsPage([] { StreamerSettings::instance().resetGeneral(); });

	auto * startup = card(p, "Startup");
#ifdef _WIN32
	toggleRow(startup, "Start with login", "Run the streamer when you sign in", s.startWithLogin);
#else
	toggleRow(startup, "Start with login", "Run the streamer when you sign in", s.startWithLogin,
	          [] { setAutostart(StreamerSettings::instance().startWithLogin); }, "Adds an XDG autostart entry (~/.config/autostart)");
#endif
	toggleRow(startup, "Start minimized", "Start in the tray instead of opening this window", s.startMinimizedInTray);

	auto * security = card(p, "Security");
	toggleRow(security, "Ask for computer access", "Prompt when a new headset searches for computers", s.showPairingRequests);
	toggleRow(security, "Lock computer on disconnect", "Lock the session when the headset disconnects", s.lockComputer, apply);
	p.cards->addStretch();
	return p.widget;
}

// ---------------------------------------------------------------------------------------------- ABOUT

AboutPage::AboutPage(Engine & engine) :
        engine_(engine)
{
	page(this);
	auto * outer = new QVBoxLayout(this);
	outer->setContentsMargins(16, 12, 8, 10);
	outer->setSpacing(8);
	auto * content = page();
	auto * cards = new QVBoxLayout(content);
	cards->setContentsMargins(0, 0, 8, 0);
	cards->setSpacing(12);
	outer->addWidget(scrollArea(content), 1);

	auto value = [](const QString & t) {
		auto * l = new QLabel(t);
		l->setObjectName("aboutValue");
		l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		l->setTextInteractionFlags(Qt::TextSelectableByMouse);
		return l;
	};
	auto link = [](const QString & url, const QString & text) {
		auto * l = new QLabel(QString("<a href=\"%1\" style=\"color:#A58CFF; text-decoration:none\">%2</a>").arg(url, text));
		l->setObjectName("aboutValue");
		l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		l->setOpenExternalLinks(true);
		return l;
	};

	headset_ = value("Not connected");
	codec_ = value("-");
	ethernet_ = value("-");
	autoBitrate_ = value("-");
	latency_ = nullptr;

	auto * connection = card(cards, "Connection");
	row(connection, "Headset", {}, headset_);
	row(connection, "Video", {}, codec_);
	row(connection, "PC Ethernet", {}, ethernet_, "A wired connection from the PC to the router gives the most stable stream");
	row(connection, "Automatic bitrate", {}, autoBitrate_);
	const QString router = sysinfo::routerUrl();
	row(connection, "Router", "Local network only", router.isEmpty() ? value("-") : link(router, router));

	auto * system = card(cards, "This computer");
	row(system, "Graphics card", {}, value(sysinfo::graphicsCards()));
	row(system, "Hardware encoder", {}, value(sysinfo::hasHardwareEncoder() ? "Yes" : "No"),
	    "A hardware video encoder is required for multi-monitor or VR game streaming");
	row(system, "Processor", {}, value(sysinfo::processor()));
	row(system, "Memory", {}, value(sysinfo::memory()));
	row(system, "Operating system", {}, value(sysinfo::operatingSystem()));
	row(system, "Secure Boot", {}, value(sysinfo::secureBoot()),
	    "When Secure Boot is disabled in BIOS, it can solve compatibility with some games");

	auto * about = card(cards, "OpenDesk");
	row(about, "Streamer version", "Independent open-source PC streamer", value(OPENDESK_VERSION));
	row(about, "Headset app protocol", "Newest headset app version this streamer speaks", value(OPENDESK_UPSTREAM_VERSION));
	auto * credits = new QLabel(
	        "<a href=\"https://github.com/mbucchia/VirtualDesktop-OpenXR\" style=\"color:#A58CFF; text-decoration:none\">VDXR</a>"
	        " by Matthieu Bucchianeri (MIT) &middot; "
	        "<a href=\"https://github.com/Supreeeme/xrizer\" style=\"color:#A58CFF; text-decoration:none\">xrizer</a>"
	        " by Supreeeme et al. (GPL-3.0) &middot; "
	        "<a href=\"https://github.com/benaclejames/VRCFaceTracking\" style=\"color:#A58CFF; text-decoration:none\">VRCFaceTracking</a>"
	        " by benaclejames et al. (Apache-2.0)");
	credits->setObjectName("settingDescription");
	credits->setWordWrap(true);
	credits->setOpenExternalLinks(true);
	credits->setToolTip("OpenDesk XR is derived from VDXR by Matthieu Bucchianeri (MIT): an OpenXR runtime for OpenDesk.\n"
	                    "xrizer by Supreeeme and contributors: runs OpenVR games on that runtime; downloaded separately, not part of this program.\n"
	                    "VRCFaceTracking by benaclejames and contributors: its VRChat face tracking parameters, used by the built-in OSC output.");
	auto * line = new QFrame;
	line->setObjectName("rowSeparator");
	line->setFixedHeight(1);
	about->addWidget(line);
	auto * creditsTitle = new QLabel("Built on");
	creditsTitle->setObjectName("settingTitle");
	creditsTitle->setContentsMargins(0, 8, 0, 0);
	about->addWidget(creditsTitle);
	credits->setContentsMargins(0, 1, 0, 8);
	about->addWidget(credits);

	auto * buttons = new QHBoxLayout;
	auto * logs = new QPushButton("Show logs");
	connect(logs, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(engine_.logPath())); });
	auto * check = new QPushButton("Check setup...");
	connect(check, &QPushButton::clicked, this, [this] { SetupDialog(window()).exec(); });
	buttons->addWidget(check);
	buttons->addWidget(logs);
	buttons->addStretch();
	cards->addLayout(buttons);
	cards->addStretch();

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

	QStringList eth;
	for (const auto & a: engine_.computer["adapters"].toArray())
	{
		auto o = a.toObject();
		if (!o["wifi"].toBool() && !o["usb"].toBool())
			eth << QString("%1 (%2)").arg(o["ip"].toString(), o["gbit"].toBool() ? "1 Gbps+" : "< 1 Gbps");
	}
	ethernet_->setText(eth.isEmpty() ? "No" : eth.join(", "));
	autoBitrate_->setText(StreamerSettings::instance().autoAdjustBitrate ? "Yes" : "No");
}
