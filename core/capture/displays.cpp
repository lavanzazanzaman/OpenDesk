#include "displays.h"
#include "portal.h"
#include "x11_capture.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <algorithm>
#include <cmath>
#include <QSocketNotifier>
#include <format>

namespace od
{
namespace
{
const char * kscreen_rotation(int transform)
{
	switch (transform & 3)
	{
		case 1: return "left";
		case 2: return "inverted";
		case 3: return "right";
		default: return "normal";
	}
}

bool kscreen_doctor(const QStringList & args, QString * error)
{
	QProcess p;
	p.start("kscreen-doctor", args);
	if (!p.waitForFinished(5000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
	{
		if (error)
			*error = "kscreen-doctor failed: " + QString::fromUtf8(p.readAllStandardError()).trimmed();
		return false;
	}
	return true;
}
} // namespace

display_manager::display_manager(QObject * parent) :
        QObject(parent)
{
	std::string why;
	kwin = kwin_screencast::connect(&why);
	if (kwin)
	{
		notifier = new QSocketNotifier(kwin->fd(), QSocketNotifier::Read, this);
		connect(notifier, &QSocketNotifier::activated, this, [this] {
			if (kwin->take_changed())
				emit displays_changed();
		});
	}
	else
	{
		kwin_reason = QString::fromStdString(why);
		x11 = x11_session();
	}
}

display_manager::~display_manager()
{
	for (auto & [name, transform]: rotated) // temporary portrait mode ends with the session
		kscreen_doctor({QString("output.%1.rotation.%2").arg(QString::fromStdString(name), kscreen_rotation(transform))}, nullptr);
	if (kwin)
	{
		delete notifier;
		for (auto & [name, handle]: physical_streams)
			kwin->close(handle);
		for (auto & v: virtuals)
			kwin->close(v.handle);
	}
	if (portal)
		portal->close();
}

void display_manager::start(const QString & token, std::function<void(QString)> done)
{
	if (kwin || x11)
	{
		started = true;
		done({});
		return;
	}
	started = false;
	portal_displays.clear();
	if (portal)
		portal->close();
	portal = std::make_unique<screencast_portal>();
	portal->start(
	        token, true,
	        [this, done](std::vector<portal_stream> list, QString new_token) {
		        if (!new_token.isEmpty())
			        emit restore_token(new_token);
		        // positions are optional in the portal API: lay unknown ones out left to right
		        int next_x = 0;
		        for (const auto & s: list)
			        if (s.has_position)
				        next_x = std::max(next_x, s.x + s.width);
		        int n = 0;
		        for (const auto & s: list)
		        {
			        display_info i;
			        i.name = std::format("Screen {}", ++n);
			        i.description = s.source_type == 4 ? "virtual" : "monitor";
			        i.x = s.has_position ? s.x : std::exchange(next_x, next_x + s.width);
			        i.y = s.has_position ? s.y : 0;
			        i.width = i.pixel_width = s.width;
			        i.height = i.pixel_height = s.height;
			        portal_displays.emplace_back(i, s.node_id);
		        }
		        started = true;
		        done({});
	        },
	        [this, done](QString error) {
		        started = false;
		        done(error);
	        });
}

std::vector<display_info> display_manager::displays()
{
	if (kwin)
		return kwin->displays();
	if (x11)
		return x11_monitors();
	std::vector<display_info> list;
	for (const auto & [info, node]: portal_displays)
		list.push_back(info);
	return list;
}

display_manager::virtual_mode display_manager::main_monitor_mode()
{
	virtual_mode m;
	for (const auto & d: displays())
		if (!d.is_virtual && d.width > 0)
		{
			m.width = d.width;
			m.height = d.height;
			m.scale = d.pixel_width > 0 ? double(d.pixel_width) / d.width : 1;
			m.refresh_mhz = d.refresh_mhz > 0 ? d.refresh_mhz : 60000;
			break;
		}
	return m;
}

void display_manager::apply_refresh(const std::string & output, int width, int height, int refresh_mhz)
{
	// virtual outputs start at 60 Hz (or a mode KWin remembers for the name); KWin accepts custom modes on them.
	// Asynchronous (kscreen-doctor round trips): KWin announces the new geometry and displays_changed follows.
	QString name = QString::fromStdString(output);
	auto run = [this](QStringList args, std::function<void(QByteArray)> then) {
		auto * p = new QProcess(this);
		connect(p, &QProcess::finished, this, [this, p, args, then](int code) {
			p->deleteLater();
			if (code != 0)
				emit log(QString("[monitors] kscreen-doctor %1 failed (%2): %3")
				                 .arg(args.join(' ')).arg(code).arg(QString::fromUtf8(p->readAllStandardError()).trimmed()));
			if (then)
				then(p->readAllStandardOutput());
		});
		connect(p, &QProcess::errorOccurred, this, [this, args](QProcess::ProcessError e) {
			emit log(QString("[monitors] kscreen-doctor %1: process error %2").arg(args.join(' ')).arg(int(e)));
		});
		p->start("kscreen-doctor", args);
	};
	auto find_mode = [name, width, height, refresh_mhz](const QByteArray & json) -> QString {
		for (const QJsonValue o: QJsonDocument::fromJson(json).object().value("outputs").toArray())
			if (o["name"].toString() == name)
				for (const QJsonValue m: o["modes"].toArray())
					if (m["size"]["width"].toInt() == width && m["size"]["height"].toInt() == height &&
					    std::abs(m["refreshRate"].toDouble() * 1000 - refresh_mhz) < 1000)
						return m["id"].toString();
		return {};
	};
	auto set_mode = [this, run, name, width, height, refresh_mhz](const QString & id) {
		if (id.isEmpty())
			return emit log(QString("[monitors] %1: no %2x%3@%4 mode").arg(name).arg(width).arg(height).arg(refresh_mhz / 1000.0));
		emit log(QString("[monitors] %1 -> %2x%3 @%4 Hz").arg(name).arg(width).arg(height).arg(refresh_mhz / 1000.0, 0, 'f', 1));
		run({QString("output.%1.mode.%2").arg(name, id)}, nullptr);
	};
	run({"-j"}, [=](QByteArray json) {
		if (QString id = find_mode(json); !id.isEmpty())
			return set_mode(id);
		run({QString("output.%1.addCustomMode.%2.%3.%4.full").arg(name).arg(width).arg(height).arg(refresh_mhz)},
		    [=](QByteArray) { run({"-j"}, [=](QByteArray json2) { set_mode(find_mode(json2)); }); });
	});
}

std::optional<display_manager::virtual_output> display_manager::create_virtual(virtual_mode mode, QString * error)
{
	virtual_output v;
	v.mode = mode;
	for (v.number = 1;; ++v.number) // lowest free number
		if (std::none_of(virtuals.begin(), virtuals.end(), [&](const virtual_output & o) { return o.number == v.number; }))
			break;
	std::string err;
	auto handle = kwin->stream_virtual_output(std::format("Upstream{}", v.number), std::format("OpenDesk monitor {}", v.number),
	                                          mode.width, mode.height, mode.scale, &v.node, &v.name, &err);
	if (!handle)
	{
		if (error)
			*error = QString::fromStdString(err);
		return std::nullopt;
	}
	v.handle = *handle;
	// always: KWin restores the mode it remembers for this output name, which may be from another main monitor
	if (!v.name.empty())
		apply_refresh(v.name, int(std::lround(mode.width * mode.scale)), int(std::lround(mode.height * mode.scale)), mode.refresh_mhz);
	return v;
}

std::optional<std::string> display_manager::add_virtual(virtual_mode mode, QString * error)
{
	if (!kwin)
	{
		if (error)
			*error = "virtual monitors need KWin (Plasma)";
		return std::nullopt;
	}
	auto v = create_virtual(mode, error);
	if (!v)
		return std::nullopt;
	virtuals.push_back(*v);
	return v->name;
}

bool display_manager::remove_virtual()
{
	if (!kwin || virtuals.empty())
		return false;
	kwin->close(virtuals.back().handle);
	virtuals.pop_back();
	return true;
}

bool display_manager::set_portrait(const display_info & d, bool portrait, QString * error)
{
	if (kwin)
		for (auto & v: virtuals)
			if (v.name == d.name)
			{
				bool is_portrait = v.mode.height > v.mode.width;
				if (is_portrait == portrait)
					return true;
				// a virtual output has no rotation: recreate it with the sides swapped
				virtual_mode m = v.mode;
				std::swap(m.width, m.height);
				auto repl = create_virtual(m, error);
				if (!repl)
					return false;
				kwin->close(v.handle);
				v = *repl;
				return true;
			}
	if (!kwin)
	{
		if (error)
			*error = "rotating monitors needs KWin (Plasma)";
		return false;
	}
	bool is_portrait = d.height > d.width;
	if (is_portrait == portrait)
		return true;
	if (!rotated.contains(d.name))
		rotated[d.name] = d.transform;
	int target = portrait ? 1 : 0;
	if (!portrait && rotated[d.name] != 1 && rotated[d.name] != 3)
		target = rotated[d.name]; // back to how it was
	return kscreen_doctor({QString("output.%1.rotation.%2").arg(QString::fromStdString(d.name), kscreen_rotation(target))}, error);
}

std::optional<desktop_stream_setup> display_manager::open(const display_info & d, QString * error)
{
	desktop_stream_setup s;
	s.name = d.name;
	s.expected_width = d.pixel_width > 0 ? d.pixel_width : d.width;
	s.expected_height = d.pixel_height > 0 ? d.pixel_height : d.height;
	if (x11)
	{
		s.x11 = true;
		s.x11_x = d.x;
		s.x11_y = d.y;
		return s;
	}
	if (!kwin)
	{
		for (const auto & [info, node]: portal_displays)
			if (info.name == d.name)
			{
				s.node_id = node;
				s.pipewire_fd = portal->open_remote(error);
				if (s.pipewire_fd < 0)
					return std::nullopt;
				return s;
			}
		if (error)
			*error = QString("%1 is not shared").arg(QString::fromStdString(d.name));
		return std::nullopt;
	}
	for (const auto & v: virtuals)
		if (v.name == d.name)
		{
			s.node_id = v.node.node_id;
			s.serial = v.node.serial;
			return s;
		}
	if (auto it = physical_streams.find(d.name); it != physical_streams.end())
	{
		kwin->close(it->second);
		physical_streams.erase(it);
	}
	stream_node node;
	std::string err;
	auto handle = kwin->stream_output(d.name, &node, &err);
	if (!handle)
	{
		if (error)
			*error = QString::fromStdString(err);
		return std::nullopt;
	}
	physical_streams[d.name] = *handle;
	s.node_id = node.node_id;
	s.serial = node.serial;
	return s;
}

void display_manager::release_unused(const std::set<std::string> & in_use)
{
	if (!kwin)
		return;
	for (auto it = physical_streams.begin(); it != physical_streams.end();)
		if (!in_use.contains(it->first))
		{
			kwin->close(it->second);
			it = physical_streams.erase(it);
		}
		else
			++it;
}
} // namespace od
