// Self-update for the AppImage: looks for a newer GitHub release, downloads it next to the running file, checks its
// SHA-256 against the release's SHA256SUMS and swaps it in. The new version starts when the user restarts the streamer.
// Other install types (the native binary, distro packages) are updated by whatever installed them.
#pragma once

#include <QFile>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkReply;

class Updater : public QObject
{
	Q_OBJECT
public:
	enum class State
	{
		Unsupported, // not running from a writable AppImage
		Idle,
		Checking,
		UpToDate,
		Available,
		Downloading,
		Ready, // the file has been replaced: restart to use it
		Failed,
	};

	static Updater & instance();

	State state() const { return state_; }
	QString statusText() const; // one line for the settings page
	QString newVersion() const { return newVersion_; }

	// install = true: also download a newer release when one is found
	void check(bool install);
	void download();
	void restart();

signals:
	void changed();

private:
	explicit Updater(QObject * parent = nullptr);
	void set(State state, const QString & detail = {});
	void fetchChecksum();
	void fetchFile(const QByteArray & expected);
	void finishFile(const QByteArray & expected);
	void fail(const QString & why);

	QNetworkAccessManager net_;
	QNetworkReply * reply_ = nullptr;
	QFile file_;
	QCryptographicHash hash_{QCryptographicHash::Sha256};
	State state_ = State::Idle;
	QString detail_, newVersion_, appImage_;
	QUrl fileUrl_, sumsUrl_;
	bool installWhenFound_ = false;
	int percent_ = 0;
};
