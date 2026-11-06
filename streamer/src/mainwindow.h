// "OpenDesk Streamer" main window: frameless dark window with header, left tab strip and footer,
// like Upstream.Streamer.MainWindow.
#pragma once

#include "engine.h"

#include <QSet>
#include <QSystemTrayIcon>
#include <QWidget>

class AccountsPage;
class GlobalShortcuts;
class QButtonGroup;
class QStackedWidget;

class MainWindow : public QWidget
{
	Q_OBJECT

public:
	explicit MainWindow(Engine &);
	void showFromTray();
	bool screenshotTabs(const QString & dir);

protected:
	void paintEvent(QPaintEvent *) override;
	void closeEvent(QCloseEvent *) override;
	bool eventFilter(QObject *, QEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mousePressEvent(QMouseEvent *) override;

private:
	QWidget * makeHeader();
	QWidget * makeFooter();
	Qt::Edges edgesAt(QPoint) const;
	void updateTray(Engine::Status);
	void onPairingRequest(const QString & account, int platform, const QString & host);

	Engine & engine_;
	QSystemTrayIcon tray_;
	QStackedWidget * stack_;
	QButtonGroup * tabs_;
	AccountsPage * accounts_;
	QWidget * header_;
	QSet<QString> askedAccounts_;
	GlobalShortcuts * shortcuts_;
	QMap<QString, QString> boundKeys_;
};
