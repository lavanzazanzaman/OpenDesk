// The six tabs of the Streamer window (layouts follow MainWindow.xaml of Upstream.Streamer).
#pragma once

#include <QWidget>

class Engine;
class QLabel;
class QVBoxLayout;

QWidget * makeBindingsPage();
QWidget * makeOptionsPage(Engine &);
QWidget * makeAdvancedPage(Engine &);

class AccountsPage : public QWidget
{
	Q_OBJECT
public:
	explicit AccountsPage(Engine &);
	void rebuild();
	void addAccount(int platform, const QString & account);

private:
	bool editing_;
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
