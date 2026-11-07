// First-run / on-demand system check: firewall ports, uinput access, capture permission, audio, encoder.
// Fixes that need root are collected and run in one `pkexec sh -c ...` (a single polkit password prompt).
#pragma once

#include <QDialog>
#include <QList>
#include <QStringList>
#include <functional>

struct SetupItem
{
	QString title;
	bool ok = false;
	QString detail;         // what's wrong / how it was detected
	QStringList rootFix;    // shell commands run as root by "Fix automatically"; empty = manual only
	std::function<QString()> userFix; // fix without root; returns an error message or empty
	QString fixedKey; // set when the result can't be read back without root: remembered once rootFix succeeded
};

QList<SetupItem> runSetupChecks();

class QVBoxLayout;

class SetupDialog : public QDialog
{
	Q_OBJECT
public:
	explicit SetupDialog(QWidget * parent = nullptr);
	bool hasProblems() const;

private:
	void refresh();
	void fix();

	QList<SetupItem> items_;
	QVBoxLayout * list_;
	class QLabel * summary_ = nullptr;
	class QPushButton * fixButton_;
};
