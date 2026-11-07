#include "mainwindow.h"

#include <algorithm>
#include "pages.h"
#include "settings.h"
#include "setup.h"
#include "shortcuts.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QTimer>
#include <QWindow>

namespace
{
constexpr int resizeBorder = 8; // WindowChrome ResizeBorderThickness="8"

QPushButton * imageButton(const QString & asset, const QString & tooltip)
{
	auto * b = new QPushButton;
	b->setObjectName("imageButton");
	b->setIcon(QIcon(":/assets/opendesk/" + asset));
	b->setIconSize({16, 16});
	b->setFixedSize(36, 30);
	b->setToolTip(tooltip);
	b->setCursor(Qt::ArrowCursor);
	return b;
}
} // namespace

MainWindow::MainWindow(Engine & engine) :
        engine_(engine)
{
	setWindowTitle("OpenDesk Streamer");
	setWindowIcon(QIcon(":/assets/opendesk/app.ico"));
	setWindowFlag(Qt::FramelessWindowHint);
	setAttribute(Qt::WA_TranslucentBackground); // rounded corners
	setMinimumSize(760, 560);
	resize(820, 620);
	setMouseTracking(true);

	auto * root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);
	root->addWidget(header_ = makeHeader());

	auto * body = new QWidget;
	body->setObjectName("body");
	body->setAttribute(Qt::WA_StyledBackground);
	auto * bodyLayout = new QHBoxLayout(body);
	bodyLayout->setContentsMargins(0, 0, 0, 0);
	bodyLayout->setSpacing(0);

	auto * strip = new QWidget;
	strip->setObjectName("tabStrip");
	strip->setAttribute(Qt::WA_StyledBackground);
	auto * stripLayout = new QVBoxLayout(strip);
	stripLayout->setContentsMargins(0, 8, 0, 0); // ACCOUNTS tab has Padding="16 16 0 0", Height 56
	stripLayout->setSpacing(0);
	tabs_ = new QButtonGroup(this);
	stack_ = new QStackedWidget;

	accounts_ = new AccountsPage(engine_);
	const QList<std::pair<QString, QWidget *>> pages{
	        {"ACCOUNTS", accounts_},
	        {"STREAMING", makeStreamingPage(engine_)},
	        {"VR", makeVRPage(engine_)},
	        {"AUDIO && INPUT", makeAudioInputPage(engine_)},
	        {"SHORTCUTS", makeShortcutsPage()},
	        {"GENERAL", makeGeneralPage(engine_)},
	        {"ABOUT", new AboutPage(engine_)},
	};
	for (int i = 0; i < pages.size(); ++i)
	{
		auto * b = new QPushButton(pages[i].first);
		b->setObjectName("tabButton");
		b->setCheckable(true);
		b->setCursor(Qt::PointingHandCursor);
		tabs_->addButton(b, i);
		stripLayout->addWidget(b);
		stack_->addWidget(pages[i].second);
	}
	stripLayout->addStretch();
	connect(tabs_, &QButtonGroup::idClicked, this, [this](int id) {
		stack_->setCurrentIndex(id);
		StreamerSettings::instance().selectedTab = id;
		StreamerSettings::instance().save();
	});
	int tab = std::clamp(StreamerSettings::instance().selectedTab, 0, int(pages.size()) - 1);
	if (!StreamerSettings::instance().hasValidAccountID())
		tab = 0;
	tabs_->button(tab)->setChecked(true);
	stack_->setCurrentIndex(tab);

	bodyLayout->addWidget(strip);
	bodyLayout->addWidget(stack_, 1);
	root->addWidget(body, 1);
	root->addWidget(makeFooter());

	// resize handles live on the window edges; children must not swallow the cursor there
	qApp->installEventFilter(this);

	// tray icon (disconnected / connected to server / connected to peer, as on Windows)
	auto * menu = new QMenu(this);
	menu->addAction("Show", this, &MainWindow::showFromTray);
	menu->addSeparator();
	menu->addAction("Exit", this, [this] {
		// Plasma serves this menu over D-Bus (DBusMenu) and waits for our reply; quitting inside the handler
		// made shutdown's blocking tray-unregister call wait on Plasma until the D-Bus timeout (~25 s each).
		// Hide the tray icon and quit on the next event-loop turn instead.
		hide();
		tray_.hide();
		QTimer::singleShot(0, qApp, &QApplication::quit);
	});
	tray_.setContextMenu(menu);
	connect(&tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
		if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick)
			isVisible() ? hide() : showFromTray();
	});
	updateTray(engine_.status());
	tray_.show();

	connect(&engine_, &Engine::statusChanged, this, &MainWindow::updateTray);
	connect(&engine_, &Engine::pairingRequest, this, &MainWindow::onPairingRequest);
	connect(&engine_, &Engine::screenshotSaved, this, [this](const QString & path) {
		tray_.showMessage("OpenDesk Streamer", "Screenshot saved to " + path);
	});
	connect(&engine_, &Engine::headsetNewer, this, [this](const QString & supported) {
		tray_.showMessage("OpenDesk Streamer",
		                  "The headset's OpenDesk app was updated past " + supported +
		                          ".\nIt may still connect; a new minor version will be refused until the streamer is updated.",
		                  QSystemTrayIcon::Warning);
	});
	connect(&engine_, &Engine::captureError, this, [this](const QString & message) {
		tray_.showMessage("OpenDesk Streamer",
		                  "Screen capture failed: " + message + "\nReconnect the headset to try again.", QSystemTrayIcon::Warning);
	});
	// displays switched off/on or plugged: tell the engine so capture can recover
	connect(qApp, &QGuiApplication::screenAdded, &engine_, &Engine::applyOptions);
	connect(qApp, &QGuiApplication::screenRemoved, &engine_, &Engine::applyOptions);

	// BINDINGS: global shortcuts through the portal; (re)bound when a key assignment changes
	shortcuts_ = new GlobalShortcuts(this);
	connect(shortcuts_, &GlobalShortcuts::activated, &engine_, &Engine::headsetAction);
	auto rebind = [this] {
		const auto & b = StreamerSettings::instance().bindings;
		bool any = std::any_of(b.begin(), b.end(), [](const QString & k) { return !k.isEmpty(); });
		if (b != boundKeys_ && (any || !boundKeys_.isEmpty()))
			shortcuts_->bind();
		boundKeys_ = b;
	};
	rebind();

	connect(&StreamerSettings::instance(), &StreamerSettings::changed, this, [this, rebind] {
		engine_.sendAccounts(StreamerSettings::instance().allAccountIDs());
		engine_.applyOptions(); // live options (paths, audio mode, ...) follow every saved change
		rebind();
	});
}

QWidget * MainWindow::makeHeader()
{
	auto * header = new QWidget;
	header->setObjectName("header");
	header->setAttribute(Qt::WA_StyledBackground);
	header->setFixedHeight(48);
	header->installEventFilter(this);
	auto * h = new QHBoxLayout(header);
	h->setContentsMargins(8, 0, 4, 0);
	h->setSpacing(8);

	auto * logo = new QLabel("OpenDesk");
	logo->setObjectName("logo");
	logo->setAttribute(Qt::WA_TransparentForMouseEvents);
	auto * suffix = new QLabel("Streamer");
	suffix->setObjectName("linuxSuffix");
	suffix->setAttribute(Qt::WA_TransparentForMouseEvents);
	h->addWidget(logo);
	h->addWidget(suffix);
	h->addStretch();

	auto * minimize = imageButton("minimize32.png", "Minimize");
	auto * close = imageButton("close32.png", "Close");
	connect(minimize, &QPushButton::clicked, this, &QWidget::showMinimized);
	connect(close, &QPushButton::clicked, this, &QWidget::close);
	h->addWidget(minimize, 0, Qt::AlignTop);
	h->addWidget(close, 0, Qt::AlignTop);
	h->setAlignment(minimize, Qt::AlignTop);
	minimize->setContentsMargins(0, 4, 0, 0);
	return header;
}

QWidget * MainWindow::makeFooter()
{
	auto * footer = new QWidget;
	footer->setObjectName("footer");
	footer->setAttribute(Qt::WA_StyledBackground);
	footer->setFixedHeight(48);
	auto * h = new QHBoxLayout(footer);
	h->setContentsMargins(16, 0, 16, 0);
	h->setSpacing(6);
	auto pair = [&](const QString & name, const QString & version, const QString & tooltip) {
		auto * key = new QLabel(name);
		key->setObjectName("footerKey");
		key->setToolTip(tooltip);
		auto * value = new QLabel(version);
		value->setObjectName("footerValue");
		value->setToolTip(tooltip);
		h->addWidget(key);
		h->addWidget(value);
	};
	pair("Streamer:", OPENDESK_VERSION, "Version of this streamer");
	h->addStretch();
	pair("Streamer Support Version:", OPENDESK_UPSTREAM_VERSION,
	     "The OpenDesk streamer version this speaks the protocol of; the headset app must be exactly this version");
	return footer;
}

void MainWindow::paintEvent(QPaintEvent *)
{
	// the window is translucent; header/body/footer paint themselves, this only clips the corners
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	QPainterPath path;
	path.addRoundedRect(rect(), 10, 10);
	p.fillPath(path, QColor("#15131C"));
}

void MainWindow::closeEvent(QCloseEvent * e)
{
	// like the Windows streamer: closing the window keeps the streamer running in the tray
	if (QSystemTrayIcon::isSystemTrayAvailable())
	{
		hide();
		e->ignore();
	}
}

bool MainWindow::screenshotTabs(const QString & dir)
{
	show();
	for (int i = 0; i < stack_->count(); ++i)
	{
		tabs_->button(i)->setChecked(true);
		stack_->setCurrentIndex(i);
		QApplication::processEvents();
		if (!grab().save(QString("%1/tab-%2.png").arg(dir).arg(i)))
			return false;
	}
	SetupDialog check(this);
	check.show();
	QApplication::processEvents();
	return check.grab().save(dir + "/setup.png");
}

void MainWindow::showFromTray()
{
	show();
	raise();
	activateWindow();
}

Qt::Edges MainWindow::edgesAt(QPoint pos) const
{
	Qt::Edges edges;
	if (pos.x() < resizeBorder)
		edges |= Qt::LeftEdge;
	if (pos.x() >= width() - resizeBorder)
		edges |= Qt::RightEdge;
	if (pos.y() < resizeBorder)
		edges |= Qt::TopEdge;
	if (pos.y() >= height() - resizeBorder)
		edges |= Qt::BottomEdge;
	return edges;
}

bool MainWindow::eventFilter(QObject * obj, QEvent * ev)
{
	if (ev->type() == QEvent::MouseMove || ev->type() == QEvent::MouseButtonPress)
	{
		auto * w = qobject_cast<QWidget *>(obj);
		if (w && w->window() == this)
		{
			auto * me = static_cast<QMouseEvent *>(ev);
			QPoint pos = mapFromGlobal(me->globalPosition().toPoint());
			Qt::Edges edges = edgesAt(pos);
			if (ev->type() == QEvent::MouseMove && !(me->buttons() & Qt::LeftButton))
			{
				Qt::CursorShape shape = Qt::ArrowCursor;
				if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge))
					shape = Qt::SizeFDiagCursor;
				else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge))
					shape = Qt::SizeBDiagCursor;
				else if (edges & (Qt::LeftEdge | Qt::RightEdge))
					shape = Qt::SizeHorCursor;
				else if (edges & (Qt::TopEdge | Qt::BottomEdge))
					shape = Qt::SizeVerCursor;
				if (edges)
					setCursor(shape);
				else
					unsetCursor();
			}
			if (ev->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton)
			{
				if (edges && windowHandle()->startSystemResize(edges))
					return true;
				if (obj == header_ && windowHandle()->startSystemMove())
					return true;
			}
		}
	}
	return QWidget::eventFilter(obj, ev);
}

void MainWindow::mouseMoveEvent(QMouseEvent * e)
{
	QWidget::mouseMoveEvent(e);
}

void MainWindow::mousePressEvent(QMouseEvent * e)
{
	QWidget::mousePressEvent(e);
}

void MainWindow::updateTray(Engine::Status status)
{
	switch (status)
	{
		case Engine::Status::Stopped:
			tray_.setIcon(QIcon(":/assets/opendesk/disconnected.ico"));
			tray_.setToolTip("OpenDesk Streamer - not running");
			break;
		case Engine::Status::Listening:
			tray_.setIcon(QIcon(":/assets/opendesk/connectedtoserver.ico"));
			tray_.setToolTip("OpenDesk Streamer - waiting for headset");
			break;
		case Engine::Status::Connected:
			tray_.setIcon(QIcon(":/assets/opendesk/connectedtopeer.ico"));
			tray_.setToolTip("OpenDesk Streamer - connected");
			break;
	}
}

void MainWindow::onPairingRequest(const QString & account, int platform, const QString & host)
{
	auto & s = StreamerSettings::instance();
	if (!s.showPairingRequests || askedAccounts_.contains(account))
		return;
	askedAccounts_.insert(account); // like _pairingRequests: ask once per account per session

	QDialog dlg(isVisible() ? this : nullptr);
	dlg.setObjectName("vdDialog");
	dlg.setWindowTitle("OpenDesk Streamer");
	auto * v = new QVBoxLayout(&dlg);
	auto * title = new QLabel("A headset is searching for computers");
	title->setObjectName("platformName");
	auto * text = new QLabel(QString("%1 account \"%2\" (%3) wants to access this computer.\nAllow it to connect?")
	                                 .arg(platformName(platform), account, host));
	text->setObjectName("aboutValue");
	auto * dontAsk = new QCheckBox("Don't ask me again");
	auto * buttons = new QDialogButtonBox;
	buttons->addButton("Allow", QDialogButtonBox::AcceptRole);
	buttons->addButton("Deny", QDialogButtonBox::RejectRole);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	v->addWidget(title);
	v->addWidget(text);
	v->addWidget(dontAsk);
	v->addWidget(buttons);
	if (!isVisible())
		tray_.showMessage("OpenDesk Streamer", "A headset is searching for computers");

	bool allow = dlg.exec() == QDialog::Accepted;
	if (dontAsk->isChecked())
		s.showPairingRequests = false;
	if (allow)
		accounts_->addAccount(platform, account); // saves -> accounts pushed to the engine
	else if (dontAsk->isChecked())
		s.save();
}
