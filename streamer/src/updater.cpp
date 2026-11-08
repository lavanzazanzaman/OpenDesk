#include "updater.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QVersionNumber>

#include <cstdio>

namespace
{
const char * const latest_release = "https://api.github.com/repos/lavanzazanzaman/OpenDesk/releases/latest";
const char * const asset_name = "OpenDesk-Linux-x86_64.AppImage";
const char * const sums_name = "SHA256SUMS";

QNetworkRequest request(const QUrl & url)
{
	QNetworkRequest r(url);
	r.setHeader(QNetworkRequest::UserAgentHeader, "OpenDesk-Streamer/" OPENDESK_VERSION);
	r.setTransferTimeout(60000);
	return r;
}

// only ever follow a release asset on GitHub (OPENDESK_UPDATE_URL: the test server)
bool trusted(const QUrl & url)
{
	return qEnvironmentVariableIsSet("OPENDESK_UPDATE_URL") || (url.scheme() == "https" && url.host() == "github.com");
}

QVersionNumber parse(QString tag)
{
	if (tag.startsWith('v'))
		tag.remove(0, 1);
	return QVersionNumber::fromString(tag).normalized();
}
} // namespace

Updater & Updater::instance()
{
	static Updater * updater = new Updater(qApp); // (destroyed with the application, before its network plugins go)
	return *updater;
}

Updater::Updater(QObject * parent) :
        QObject(parent)
{
	appImage_ = qEnvironmentVariable("APPIMAGE");
	const QFileInfo info(appImage_);
	if (appImage_.isEmpty() || !info.isFile() || !info.isWritable() || !QFileInfo(info.absolutePath()).isWritable())
		state_ = State::Unsupported;
}

QString Updater::statusText() const
{
	switch (state_)
	{
		case State::Unsupported:
			return qEnvironmentVariableIsSet("APPIMAGE") ? "The AppImage's folder is not writable, so it can't update itself"
			                                             : "Only the AppImage updates itself; update this install the way you installed it";
		case State::Idle:
			return "Version " OPENDESK_VERSION;
		case State::Checking:
			return "Checking for updates...";
		case State::UpToDate:
			return "Version " OPENDESK_VERSION " is the latest";
		case State::Available:
			return QString("Version %1 is available").arg(newVersion_);
		case State::Downloading:
			return QString("Downloading version %1 (%2%)").arg(newVersion_).arg(percent_);
		case State::Ready:
			return QString("Version %1 is ready: restart the streamer to use it").arg(newVersion_);
		case State::Failed:
			return "Update failed: " + detail_;
	}
	return {};
}

void Updater::set(State state, const QString & detail)
{
	state_ = state;
	detail_ = detail;
	emit changed();
}

void Updater::fail(const QString & why)
{
	qWarning("[update] %s", qPrintable(why));
	if (file_.isOpen())
		file_.remove();
	set(State::Failed, why);
}

void Updater::check(bool install)
{
	if (state_ == State::Unsupported || state_ == State::Checking || state_ == State::Downloading || state_ == State::Ready)
		return;
	installWhenFound_ = install;
	set(State::Checking);
	const QByteArray override_url = qgetenv("OPENDESK_UPDATE_URL");
	auto * reply = net_.get(request(QUrl(override_url.isEmpty() ? QByteArray(latest_release) : override_url)));
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (http == 404) // no release published yet
			return set(State::UpToDate);
		if (reply->error() != QNetworkReply::NoError)
			return fail(reply->errorString());
		const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
		const QString tag = release["tag_name"].toString();
		const QVersionNumber latest = parse(tag);
		if (latest.isNull())
			return fail("the latest release has no version number");
		if (latest <= QVersionNumber::fromString(OPENDESK_VERSION).normalized())
			return set(State::UpToDate);
		fileUrl_ = sumsUrl_ = QUrl();
		const QJsonArray assets = release["assets"].toArray();
		for (const auto & a: assets)
		{
			const QJsonObject asset = a.toObject();
			if (asset["name"].toString() == asset_name)
				fileUrl_ = QUrl(asset["browser_download_url"].toString());
			else if (asset["name"].toString() == sums_name)
				sumsUrl_ = QUrl(asset["browser_download_url"].toString());
		}
		if (!trusted(fileUrl_) || !trusted(sumsUrl_))
			return fail("the latest release is missing its AppImage or checksums");
		newVersion_ = tag.startsWith('v') ? tag.mid(1) : tag;
		qInfo("[update] version %s is available", qPrintable(newVersion_));
		set(State::Available);
		if (installWhenFound_)
			download();
	});
}

void Updater::download()
{
	if (state_ != State::Available && !(state_ == State::Failed && fileUrl_.isValid()))
		return;
	percent_ = 0;
	set(State::Downloading);
	fetchChecksum();
}

void Updater::fetchChecksum()
{
	auto * reply = net_.get(request(sumsUrl_));
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError)
			return fail(reply->errorString());
		QByteArray expected;
		const QList<QByteArray> lines = reply->readAll().split('\n');
		for (const auto & line: lines)
		{
			const QList<QByteArray> parts = line.simplified().split(' ');
			if (parts.size() == 2 && parts[1] == asset_name)
				expected = parts[0].toLower();
		}
		if (expected.size() != 64)
			return fail("no checksum for the AppImage in the release");
		fetchFile(expected);
	});
}

void Updater::fetchFile(const QByteArray & expected)
{
	// same folder as the AppImage: the final rename can't cross file systems
	file_.setFileName(QFileInfo(appImage_).absolutePath() + "/." + QFileInfo(appImage_).fileName() + ".update");
	if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return fail(file_.errorString());
	hash_.reset();
	QNetworkRequest r = request(fileUrl_);
	r.setTransferTimeout(120000);
	reply_ = net_.get(r);
	connect(reply_, &QIODevice::readyRead, this, [this] {
		const QByteArray data = reply_->readAll();
		hash_.addData(data);
		if (file_.write(data) != data.size())
			reply_->abort();
	});
	connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 done, qint64 total) {
		const int p = total > 0 ? int(done * 100 / total) : 0;
		if (p != percent_)
		{
			percent_ = p;
			emit changed();
		}
	});
	connect(reply_, &QNetworkReply::finished, this, [this, expected] { finishFile(expected); });
}

void Updater::finishFile(const QByteArray & expected)
{
	reply_->deleteLater();
	const QNetworkReply::NetworkError error = reply_->error();
	const QString message = reply_->errorString();
	reply_ = nullptr;
	if (error != QNetworkReply::NoError)
		return fail(message);
	if (hash_.result().toHex() != expected)
		return fail("the download's checksum does not match");
	file_.setPermissions(QFile(appImage_).permissions() | QFile::ExeOwner | QFile::ExeUser);
	file_.close();
	// rename: the running AppImage keeps its old file until it exits
	if (std::rename(QFile::encodeName(file_.fileName()).constData(), QFile::encodeName(appImage_).constData()) != 0)
	{
		QFile::remove(file_.fileName());
		return fail("could not replace the AppImage");
	}
	qInfo("[update] version %s installed, used from the next start", qPrintable(newVersion_));
	set(State::Ready);
}

void Updater::restart()
{
	if (state_ != State::Ready)
		return;
	// a clean start: nothing of this mount's library setup, and a moment for this instance's lock to go away
	QProcess p;
	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	for (const char * name: {"LD_LIBRARY_PATH", "OPENDESK_APPIMAGE_LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "APPDIR",
	                         "APPIMAGE", "ARGV0", "OWD"})
		env.remove(name);
	const QByteArray original = qgetenv("OPENDESK_APPIMAGE_LD_LIBRARY_PATH");
	if (!original.isEmpty())
		env.insert("LD_LIBRARY_PATH", QString::fromLocal8Bit(original));
	p.setProcessEnvironment(env);
	p.setProgram("sh");
	p.setArguments({"-c", "sleep 2; exec \"$0\"", appImage_});
	if (!p.startDetached())
		return fail("could not start the new version");
	QApplication::quit();
}
