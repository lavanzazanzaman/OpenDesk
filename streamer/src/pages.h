// The tabs of the Streamer window: accounts, settings by category, about.
#pragma once

#include <QWidget>

class Engine;
class QLabel;
class QVBoxLayout;

QWidget * makeShortcutsPage();
QWidget * makeStreamingPage(Engine &);
QWidget * makeVRPage(Engine &);
QWidget * makeAudioInputPage(Engine &);
QWidget * makeGeneralPage(Engine &);
QWidget * makeWhatsNewPage();

class AccountsPage : public QWidget
{
	Q_OBJECT
public:
	explicit AccountsPage(Engine &);
	void rebuild();
	void addAccount(int platform, const QString & account);

private:
	QVBoxLayout * list_;
};

class AboutPage : public QWidget
{
	Q_OBJECT
public:
	explicit AboutPage(Engine &);

private:
	void refresh();
	Engine & engine_;
	QLabel * headset_;
	QLabel * codec_;
	QLabel * latency_;
	QLabel * ethernet_;
	QLabel * autoBitrate_;
};
