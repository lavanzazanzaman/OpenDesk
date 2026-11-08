#include "adapters.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkInterface>

namespace od
{
namespace
{
QString sysfs(const QString & iface, const QString & name)
{
	QFile f("/sys/class/net/" + iface + "/" + name);
	return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}

bool is_usb_link(const QString & iface)
{
	QString driver = QFileInfo("/sys/class/net/" + iface + "/device/driver").canonicalFilePath().section('/', -1);
	return driver == "cdc_ncm" || driver == "cdc_ether" || driver == "rndis_host";
}
} // namespace

std::vector<network_adapter> local_adapters()
{
	static const QStringList skip{"lo", "docker", "br-", "virbr", "veth", "tun", "tap", "wg"};
	std::vector<network_adapter> out;
	for (const QNetworkInterface & ni: QNetworkInterface::allInterfaces())
	{
		const QString name = ni.name();
		if (std::any_of(skip.begin(), skip.end(), [&](const QString & p) { return name.startsWith(p); }))
			continue;
		if (!(ni.flags() & QNetworkInterface::IsUp) || !(ni.flags() & QNetworkInterface::IsRunning))
			continue;

		network_adapter base;
		base.mac = ni.hardwareAddress().remove(':').toUpper().toStdString();
		base.is_wireless = QFileInfo::exists("/sys/class/net/" + name + "/wireless");
		base.is_gigabit = sysfs(name, "speed").toInt() >= 1000;
		base.is_usb = is_usb_link(name);

		for (const QNetworkAddressEntry & e: ni.addressEntries())
		{
			const QHostAddress & ip = e.ip();
			network_adapter a = base;
			if (ip.protocol() == QAbstractSocket::IPv4Protocol && !base.is_usb)
				a.ip = ip.toString().toStdString();
			else if (ip.protocol() == QAbstractSocket::IPv6Protocol && base.is_usb && ip.isLinkLocal())
			{
				QHostAddress bare = ip;
				bare.setScopeId({});
				a.ip = bare.toString().toStdString();
			}
			else
				continue;
			out.push_back(std::move(a));
		}
	}
	return out;
}
} // namespace od
