/**
 * @file unit.hpp
 * @brief Vault unit option parsing and file export/import support
 * @author Denis Zalevskiy <denis.zalevskiy@jolla.com>
 * @copyright (C) 2014 Jolla Ltd.
 * @par License: LGPL 2.1 http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
 */

#include "unit.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <utime.h>
#include <memory>
#include <functional>

#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QString>
#include <QProcess>
#include <QDebug>
#include <QLoggingCategory>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>

Q_LOGGING_CATEGORY(lcBackup, "org.sailfishos.backup", QtWarningMsg)

namespace vault { namespace unit {

static const unsigned current_version = 1;
static const QString config_prefix = ".f8b52b7481393a3e6ade051ecfb549fa";

namespace {

const QList <QCommandLineOption> options_info =
  {QCommandLineOption({"d", "dir"},      "dir",     "undef"),
   QCommandLineOption({"b", "bin-dir"},  "bin-dir", "undef"),
   QCommandLineOption({"H", "home-dir"}, "home",    "undef"),
   QCommandLineOption({"n", "name"},     "appname", "undef"),
   QCommandLineOption({"a", "action"},   "action",  "undef")};

// QCommandLineParser can't handle whitespaces, thus do preprocessing.
QStringList parseArgs()
{
    auto args = QCoreApplication::arguments();
    QStringList argsWithSpace = {args[0]};
    QString withSpace;
    QString argname;
    for (int i = 1; i < args.size(); ++i) {
        QString arg = args[i];
        if (arg.at(0) == '-') {
            if (!withSpace.isEmpty()) {
                argsWithSpace << argname << withSpace.remove(withSpace.size()-1, 1);
                withSpace.clear();
            }
            argname = arg;
        } else if (i == args.size()-1) {
            withSpace += arg;
            argsWithSpace << argname << withSpace;
        } else {
            withSpace += arg + " ";
        }
    }
    return argsWithSpace;
}

// cp -L -p -f
void copy(QString const &src, QString dst)
{
    bool failure = false;
    if (QFileInfo(dst).isDir())
        dst += QDir::separator() + QFileInfo(src).fileName();
    if (QFile(dst).exists())
        failure |= !QFile::remove(dst);

    struct stat statbuf;
    failure |= stat(src.toStdString().c_str(), &statbuf);
    const struct utimbuf timestamp = {statbuf.st_atime, statbuf.st_mtime};

    if (QFile::copy(src, dst)) {
        QFile(dst).setPermissions(QFile(src).permissions());
        failure |= chown(dst.toStdString().c_str(), statbuf.st_uid, statbuf.st_gid);
        failure |= utime(dst.toStdString().c_str(), &timestamp);
    } else {
        failure = true;
    }
    if (failure)
        qCWarning(lcBackup) << "Copy failed:" << src << dst;
}

void cptree(QString const &src, QString dst, bool update=false, bool first=true)
{

    if (first && QFileInfo(src).baseName() != QFileInfo(dst).baseName())
        dst += "/" + QFileInfo(src).baseName();

    QDir dir(src);
    if (!dir.exists())
        return;
    QDir().mkpath(dst);

    for (auto &d : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString dir = QDir::separator() + d;
        cptree(src+dir, dst+dir, update, false);
    }
    for (auto &f : dir.entryList(QDir::Files)) {
        QString file = QDir::separator() + f;
        if ((!update || !QFileInfo(dst+file).exists()) ||
            (QFileInfo(src+file).lastModified() >= QFileInfo(dst+file).lastModified())) {
            copy(src+file, dst+file);
        }
    }
}

void update_tree(QString const &src, QString const &dst)
{
    cptree(src, dst, true);
}

typedef QVariantMap map_type;
typedef QList<QVariantMap> list_type;

class Version {
public:
    Version(QString const &root)
        : fname(QFileInfo(root + "/" + config_prefix + ".unit.version").absoluteFilePath())
    {}
    unsigned get()
    {
        QFile f(fname);
        if (!f.open(QIODevice::ReadOnly))
            qCWarning(lcBackup) << "Can't open file" << fname;
        return f.exists()
            ? QString(f.readAll()).toInt()
            : 0;
    }
    void save()
    {
        QFile f(fname);
        if (f.open(QIODevice::WriteOnly))
            f.write(std::to_string(current_version).c_str());
        else
            qCWarning(lcBackup) << "Can't open file" << fname;
    }
private:
    QString fname;
};

class Operation
{
public:
    Operation(map_type const &c) : context(c)
    {
        parser.addOptions(options_info);
        parser.process(parseArgs());
        vault_dir = {{"bin", parser.value("bin-dir")}, {"data", parser.value("dir")}};
        home = QFileInfo(parser.value("home-dir")).canonicalFilePath();
    }

    void execute();
private:

    typedef std::function<void (QString const &, list_type &
                                , map_type const &)> action_type;

    class Links
    {
    public:
        Links(map_type &&d, QString const &root)
            : data(std::move(d))
            , root_dir(root)
        {}

        void add(map_type const &info) {
            map_type value = {{ "target", info["target"]}
                              , {"target_path", info["target_path"]}};
            data.insert(info["path"].toString(), std::move(value));
        }

        void save() {
            if (!data.empty())
                write_links(data, root_dir);
        }

        map_type get(map_type const &info) {
            return data[info["path"].toString()].toMap();
        }

        map_type data;
        QString root_dir;
    };

    void to_vault(QString const &data_type
                  , list_type &paths
                  , map_type const &location);

    void from_vault(QString const &data_type
                    , list_type &paths
                    , map_type const &location);

    static map_type read_links(QString const &root_dir) {
        QFile f(get_link_info_fname(root_dir));
        if (!f.open(QFile::ReadOnly)) {
            qCWarning(lcBackup) << "Unable to open" << f.fileName();
            return map_type();
        }
        auto data = f.readAll();
        return QJsonDocument::fromJson(data).object().toVariantMap();
    }

    static size_t write_links(map_type const &links, QString const &root_dir)
    {
        QFile f(get_link_info_fname(root_dir));
        if (!f.open(QFile::WriteOnly)) {
            qCWarning(lcBackup) << "Can't write to" << f.fileName();
            return 0;
        }
        return f.write(QJsonDocument::fromVariant(links).toJson());
    }

    static QString get_link_info_fname(QString const &root)
    {
        return QFileInfo(root + "/" + config_prefix + ".links").absoluteFilePath();
    }

    Version version(QString const &root)
    {
        return Version(root);
    }

    QString get_root_vault_dir(QString const &data_type);

    QCommandLineParser parser;
    map_type const &context;
    map_type vault_dir;
    QString home;
};

void create_dst_dirs(map_type const &item)
{
    QString path = item["full_path"].toString();
    if (!QFileInfo(item["src"].toString()).isDir())
        path = QFileInfo(path).dir().absolutePath();

    if (!QFileInfo(path).isDir()) {
        if (!QDir().mkpath(path) && item["required"].toBool())
            throw std::runtime_error("Can't create path " + path.toStdString());
    }
}

QString Operation::get_root_vault_dir(QString const &data_type)
{
    QString res = vault_dir[data_type].toString();
    if (!QFileInfo(res).isDir())
        throw std::runtime_error("Vault dir doesn't exist: " + res.toStdString()
                                 + " or unknown datatype:  " + data_type.toStdString());
    return res;
}

void Operation::to_vault(QString const &data_type
                         , list_type &paths
                         , map_type const &location)
{
    qCDebug(lcBackup) << "To vault" << data_type << "Paths"
                      << paths << "Location" << location;

    QString dst_root = get_root_vault_dir(data_type);
    QString link_info_path = get_link_info_fname(dst_root);
    Links links(read_links(dst_root), dst_root);

    auto copy_entry = [dst_root](map_type const &info) {
        qCDebug(lcBackup) << "COPY" << info;
        QString dst = QFileInfo(dst_root + "/" + info["path"].toString()).absolutePath();
        QString src = info["full_path"].toString();

        if (!QFileInfo(dst).isDir() && !QDir().mkpath(dst)) {
            throw std::runtime_error("Can't create destination in vault: "
                                     + dst.toStdString());
        }
        if (QFileInfo(src).isDir()) {
            update_tree(src, dst);
        } else if (QFileInfo(src).isFile()) {
            copy(src, dst);
        } else {
            throw std::runtime_error("No handler for this entry type"
                                     + src.toStdString());
        }
    };

    auto process_symlink = [this, &links](map_type &path) {
        QString full_path = QFileInfo(path["full_path"].toString()).absoluteFilePath();
        if (!QFileInfo(full_path).isSymLink())
            return;

        QString tgt = QFileInfo(full_path).symLinkTarget();
        full_path = QFileInfo(full_path).canonicalFilePath();
        QString root_path = QFileInfo(path["root_path"].toString()).canonicalFilePath();
        QString tgt_path = QDir(home).relativeFilePath(full_path);

        qCDebug(lcBackup) << "Process symlink" << path;
        if (!full_path.contains(QRegExp("^" + root_path))) {
            if (path["required"].toBool())
                throw std::runtime_error("Required path does not belong to its root dir: "
                                         + full_path.toStdString());
            path["skip"] = true;
            return;
        }

        map_type link_info(path);
        link_info.unite(path);
        link_info["target"] = tgt;
        link_info["target_path"] = tgt_path;
        qCDebug(lcBackup) << "Symlink info" << link_info;
        links.add(link_info);

        path["full_path"] = full_path;
        path["path"] = tgt_path;
    };

    auto is_src_exists = [](map_type const &info) {
        auto res = true;
        auto full_path = info["full_path"].toString();
        if (info["skip"].toBool()) {
            res = false;
        } else if (!QFileInfo(full_path).exists()) {
            if (info["required"].toBool())
                throw std::runtime_error("Required path does not exist: " +
                                          full_path.toStdString());
            res = false;
        }
        if (!res)
            qCDebug(lcBackup) << "Does not exist/skip" << info;
        return res;
    };

    std::for_each(paths.begin(), paths.end(), process_symlink);
    list_type existing_paths;
    for (auto const &path : paths) {
        if (is_src_exists(path))
            existing_paths.push_back(path);
    }
    std::for_each(existing_paths.begin(), existing_paths.end(), copy_entry);
    links.save();
    version(dst_root).save();
}

void Operation::from_vault(QString const &data_type
                           , list_type &items
                           , map_type const &location)
{
    qCDebug(lcBackup) << "From vault" << data_type << "Paths" << items << "Location" << location;
    QString src_root(get_root_vault_dir(data_type));

    bool overwrite_default;
    {
        auto v = location["options"].toMap()["overwrite"];
        if (!v.isValid())
            v = context["options"].toMap()["overwrite"];
        overwrite_default = v.toBool();
    }

    Links links(read_links(src_root), src_root);

    auto fallback_v0 = [src_root, &items]() {
        qCDebug(lcBackup) << "Restoring from old unit version";
        if (items.empty())
            throw std::runtime_error("There should be at least 1 item");
        // during migration from initial format old (and single)
        // item is going first
        QString dst = QFileInfo(items[0]["full_path"].toString()).absoluteFilePath();
        if (!QFileInfo(dst).isDir() && !QDir().mkpath(dst))
                throw std::runtime_error("Can't create directory" + dst.toStdString());
        update_tree(src_root, dst);
    };

    auto process_absent_and_links = [src_root, &links](map_type &item) {
        QString item_path = item["path"].toString();
        QString src = QFileInfo(src_root + "/" + item_path).absoluteFilePath();
        if (QFileInfo(src).exists()) {
            item["src"] = src;
            return map_type();
        }
        map_type link = links.get(item);
        item["skip"] = true;

        auto create_link = [](map_type const &link, map_type const &item) {
            create_dst_dirs(item);
            QFile::link(link["target"].toString(), item["full_path"].toString());
        };

        if (link.empty()) {
            qCDebug(lcBackup) << "No symlink for" << item_path;
            if (item["required"].toBool()) {
                qCCritical(lcBackup) << "No source item:" << src << link["path"];
                throw std::runtime_error("No required source item");
            }
            return map_type();
        }

        qCDebug(lcBackup) << "There is a symlink for" << item_path;
        QVariantMap linked(item);
        QString target_path = link["target_path"].toString();
        linked["path"] = target_path;
        linked["full_path"] = QFileInfo(item["root_path"].toString() + "/" + target_path).absoluteFilePath();
        src = QFileInfo(src_root + "/"  + target_path).absoluteFilePath();
        if (QFileInfo(src).exists()) {
            linked["src"] = src;
            linked["skip"] = false;
            create_link(link, item);
            qCDebug(lcBackup) << "Symlink target path is" << linked;
            return linked;
        } else if (item["required"].toBool()) {
            qCCritical(lcBackup) << "No linked source item:" << "path" << src
                     << "link" << link["path"] << "target" << linked["path"];
            throw std::runtime_error("No linked source item");
        }
        return map_type();
    };

    unsigned v = version(src_root).get();
    if (v > current_version) {
        throw std::runtime_error("Can't restore from newer unit version. Expected:"
                    + std::to_string(current_version) + " got: " + std::to_string(v));
    } else if (v < current_version) {
        return fallback_v0();
    }

    list_type linked_items;
    for (auto &item : items) {
        map_type linked = process_absent_and_links(item);
        if (!linked.empty())
            linked_items.push_back(std::move(linked));
    }
    items.append(linked_items);
    qCDebug(lcBackup) << "LINKED+" << items;

    for (auto const &item : items) {
        if (item["skip"].toBool()) {
            qCDebug(lcBackup) << "Skipping" << item["path"].toString();
            continue;
        }
        bool overwrite;
        {
            auto v = item["overwrite"];
            overwrite = v.isValid() ? v.toBool() : overwrite_default;
        }

        // TODO process correctly self dir (copy with dir itself)
        create_dst_dirs(item);
        QString src = item["src"].toString();
        QString dst = item["full_path"].toString();
        QString dst_dir = QFileInfo(dst).path();
        if (QFileInfo(src).isDir()) {
            src = QFileInfo(src).canonicalFilePath();
            cptree(src, dst_dir);
        } else if (QFileInfo(src).isFile()) {
            if (overwrite)
                unlink(dst.toStdString().c_str());
            copy(src, dst_dir);
        } else {
            qCWarning(lcBackup) << "No operation done or file found:" << src;
        }
    }
}

void Operation::execute()
{
    qCDebug(lcBackup) << "Unit execute. Context:" << context;
    action_type action;

    if (!QFileInfo(home).isDir())
        throw std::runtime_error("Home dir doesn't exist" + home.toStdString());

    QString action_name = parser.value("action");
    using namespace std::placeholders;
    if (action_name == "export") {
        action = std::bind(&Operation::to_vault, this, _1, _2, _3);
    } else if (action_name == "import") {
        action = std::bind(&Operation::from_vault, this, _1, _2, _3);
    } else {
        throw std::runtime_error("Unknown action" + action_name.toStdString());
    }

    auto get_home_path = [this](QVariant const &item) {
        map_type res;
        if (static_cast<QMetaType::Type>(item.type()) == QMetaType::QString) {
            res["path"] = item.toString();
        } else if (static_cast<QMetaType::Type>(item.type()) == QMetaType::QVariantMap) {
            res.unite(item.toMap());
        } else {
            throw std::runtime_error("Unexpected path type" +
            item.toString().toStdString());
        }

        QString path = res["path"].toString();
        if (path.isEmpty())
            throw std::runtime_error("Invalid data(path)" +
                            item.toString().toStdString());

        res["full_path"] = QFileInfo(home + "/" + path).absoluteFilePath();
        res["root_path"] = home;
        return res;
    };


    auto process_home_path = [&action, get_home_path](map_type const &location) {
        auto create_paths_list = [get_home_path](auto const &items) {
            list_type l;
            for (auto const &i : items)
                 l.push_back(get_home_path(i));
            return l;
        };

        for (auto it = location.begin(); it != location.end(); ++it) {
            auto const &name = it.key();
            auto const &items = it.value();
            if (name == "options")
                continue; // skip options
            auto data_type = name;
            list_type paths = create_paths_list(items.toList());
            action(data_type, paths, location);
        };
    };

    for (auto it = context.begin(); it != context.end(); ++it) {
        auto const &name = it.key();
        auto const &value = it.value();
        if (name == "options")
            continue; // skip options

        if (name == "home") {
            process_home_path(value.toMap());
        } else {
            throw std::runtime_error("Unknown context item" + name.toStdString());
        }
    };

}

} // namespace

int execute(QVariantMap const &info)
{
    try {
        Operation op(info);
        op.execute();
    } catch (std::exception const &e) {
        qCCritical(lcBackup) << e.what();
        return 1;
    }
    return 0;
}

int runProcess(const QString &program, const QStringList &args)
{
    QProcess ps;
    ps.start(program, args);
    ps.waitForFinished(-1);
    if (ps.exitStatus() == QProcess::CrashExit || ps.exitCode() != 0) {
        qCWarning(lcBackup) << ps.program() << args << "failed";
        qCWarning(lcBackup) << ps.readAllStandardError();
        qCWarning(lcBackup) << ps.readAllStandardOutput();
        return 1;
    }
    return 0;
}

QString optValue(const QString &arg)
{
    QCommandLineParser p;
    p.addOptions(options_info);
    p.process(parseArgs());
    return p.value(arg);
}

}} // namespace vault::unit
