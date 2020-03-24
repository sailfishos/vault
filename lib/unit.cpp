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

#include <qtaround/util.hpp>
#include <qtaround/os.hpp>

#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QString>
#include <QProcess>
#include <QDebug>
#include <QLoggingCategory>
#include <QJsonObject>
#include <QJsonDocument>

namespace os = qtaround::os;
namespace error = qtaround::error;
namespace util = qtaround::util;

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
        : fname(root + "/" + config_prefix + ".unit.version")
    {}
    unsigned get()
    {
        return os::path::exists(fname)
            ? QString::fromUtf8(os::read_file(fname)).toInt()
            : 0;
    }
    void save()
    {
        os::write_file(fname, "1");
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

    typedef std::function<void (QString const &, std::unique_ptr<list_type>
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
            data.insert(str(info["path"]), std::move(value));
        }

        void save() {
            if (!data.empty())
                write_links(data, root_dir);
        }

        map_type get(map_type const &info) {
            return data[str(info["path"])].toMap();
        }

        map_type data;
        QString root_dir;
    };

    void to_vault(QString const &data_type
                  , std::unique_ptr<list_type> paths_ptr
                  , map_type const &location);

    void from_vault(QString const &data_type
                    , std::unique_ptr<list_type> paths_ptr
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
        return os::path::join(root, config_prefix + ".links");
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
    auto path = str(item["full_path"]);
    if (!os::path::isDir(str(item["src"])))
        path = os::path::dirName(path);

    if (!os::path::isDir(path)) {
        if (!os::mkdir(path, {{"parent", true}}) && is(item["required"]))
            error::raise({{"msg", "Can't recreate tree to required item"},
                        {"path", item["path"]},
                            {"dst_dir", path}});
    }
}

QString Operation::get_root_vault_dir(QString const &data_type)
{
    auto res = str(vault_dir[data_type]);
    if (res.isEmpty())
        error::raise({{"msg", "Unknown data type is unknown"},
                    {"data_type", data_type}});
    if (!os::path::isDir(res))
        error::raise({{"msg", "Vault dir doesn't exist?"}, {"dir", res }});
    return res;
}

void Operation::to_vault(QString const &data_type
                         , std::unique_ptr<list_type> paths_ptr
                         , map_type const &location)
{
    auto paths = unbox(std::move(paths_ptr));
    qCDebug(lcBackup) << "To vault" << data_type << "Paths" << paths << "Location" << location;
    auto dst_root = get_root_vault_dir(data_type);
    auto link_info_path = get_link_info_fname(dst_root);
    Links links(read_links(dst_root), dst_root);

    auto copy_entry = [dst_root](map_type const &info) {
        qCDebug(lcBackup) << "COPY" << info;
        auto dst = os::path::dirName(os::path::join(dst_root, str(info["path"])));
        auto src = str(info["full_path"]);

        if (!(os::path::isDir(dst) || os::mkdir(dst, {{ "parent", true }}))) {
            error::raise({{"msg", "Can't create destination in vault"}
                    , {"path", dst}});
        }

        if (QFileInfo(src).isDir()) {
            update_tree(src, dst);
        } else if (QFileInfo(src).isFile()) {
            copy(src, dst);
        } else {
            error::raise({{"msg", "No handler for this entry type"}, {"path", src}});
        }
    };

    auto process_symlink = [this, &links](map_type &v) {
        auto full_path = str(get(v, "full_path"));

        if (!os::path::isSymLink(full_path))
            return;

        qCDebug(lcBackup) << "Process symlink" << v;
        if (!os::path::isDescendent(full_path, str(get(v, "root_path")))) {
            if (is(get(v, "required")))
                error::raise({{"msg", "Required path does not belong to its root dir"}
                        , {"path", full_path}});
            v["skip"] = true;
            return;
        }

        // separate link and deref destination
        auto tgt = os::path::target(full_path);
        full_path = os::path::canonical(os::path::deref(full_path));
        auto tgt_path = os::path::relative(full_path, home);

        map_type link_info(v);
        link_info.unite(v);
        link_info["target"] = tgt;
        link_info["target_path"] = tgt_path;
        qCDebug(lcBackup) << "Symlink info" << link_info;
        links.add(link_info);

        v["full_path"] = full_path;
        v["path"] = tgt_path;
    };

    auto is_src_exists = [](map_type const &info) {
        auto res = true;
        auto full_path = str(info["full_path"]);
        if (is(info["skip"])) {
            res = false;
        } else if (!os::path::exists(full_path)) {
            if (is(info["required"]))
                error::raise({{"msg", "Required path does not exist"}
                        , {"path", full_path}});
            res = false;
        }
        if (!res)
            qCDebug(lcBackup) << "Does not exist/skip" << info;

        return res;
    };

    std::for_each(paths.begin(), paths.end(), process_symlink);
    list_type existing_paths;
    for (auto it = paths.begin(); it != paths.end(); ++it) {
        if (is_src_exists(*it))
            existing_paths.push_back(*it);
    }
    std::for_each(existing_paths.begin(), existing_paths.end(), copy_entry);
    links.save();
    version(dst_root).save();
}

void Operation::from_vault(QString const &data_type
                           , std::unique_ptr<list_type> paths_ptr
                           , map_type const &location)
{
    auto items = unbox(std::move(paths_ptr));
    qCDebug(lcBackup) << "From vault" << data_type << "Paths" << items << "Location" << location;
    QString src_root(get_root_vault_dir(data_type));

    bool overwrite_default;
    {
        auto v = get(location, "options", "overwrite");
        if (!v.isValid())
            v = get(context, "options", "overwrite");
        overwrite_default = is(v);
    }

    Links links(read_links(src_root), src_root);

    auto fallback_v0 = [src_root, &items]() {
        qCDebug(lcBackup) << "Restoring from old unit version";
        if (items.empty())
            error::raise({{"msg", "There should be at least 1 item"}});
        // during migration from initial format old (and single)
        // item is going first
        auto dst = os::path::join(str(items[0]["full_path"]));
        if (!os::path::isDir(dst)) {
            if (!os::mkdir(dst, {{"parent", true}}))
                error::raise({{"msg", "Can't create directory"}, {"dir", dst}});
        }
        update_tree(src_root, dst);
    };

    auto process_absent_and_links = [src_root, &links](map_type &item) {
        auto item_path = str(item["path"]);
        auto src = os::path::join(src_root, item_path);
        if (os::path::exists(src)) {
            item["src"] = src;
            return map_type();
        }
        auto link = links.get(item);
        item["skip"] = true;

        auto create_link = [](map_type const &link, map_type const &item) {
            create_dst_dirs(item);
            os::symlink(str(link["target"]), str(item["full_path"]));
        };

        if (link.empty()) {
            qCDebug(lcBackup) << "No symlink for" << item_path;
            if (is(item["required"]))
                error::raise({{"msg", "No required source item"},
                            {"path", src}, {"path", link["path"]}});
            return map_type();
        }

        qCDebug(lcBackup) << "There is a symlink for" << item_path;
        QVariantMap linked(item);
        auto target_path = str(link["target_path"]);
        linked["path"] = target_path;
        linked["full_path"] = os::path::join(str(item["root_path"]), target_path);
        src = os::path::join(src_root, target_path);
        if (os::path::exists(src)) {
            linked["src"] = src;
            linked["skip"] = false;
            create_link(link, item);
            qCDebug(lcBackup) << "Symlink target path is" << linked;
            return linked;
        } else if (is(item["required"])) {
            error::raise({{"msg", "No linked source item"},
                        {"path", src}, {"link", link["path"]}
                        , {"target", linked["path"]}});
        }
        return map_type();
    };

    auto v = version(src_root).get();
    if (v > current_version) {
        error::raise({{"msg", "Can't restore from newer unit version"
                        ", upgrade vault"},
                    {"expected", current_version}, {"actual", v}});
    } else if (v < current_version) {
        return fallback_v0();
    }

    list_type linked_items;
    for (auto it = items.begin(); it != items.end(); ++it) {
        auto linked = process_absent_and_links(*it);
        if (!linked.empty())
            linked_items.push_back(std::move(linked));
    }
    items.append(linked_items);
    qCDebug(lcBackup) << "LINKED+" << items;

    for (auto it = items.begin(); it != items.end(); ++it) {
        auto &item = *it;
        QString src, dst_dir, dst;
        if (is(item["skip"])) {
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
        src = item["src"].toString();
        dst = item["full_path"].toString();
        dst_dir = QFileInfo(dst).path();
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

    if (!os::path::isDir(home))
        error::raise({{"msg", "Home dir doesn't exist"}, {"dir", home}});

    auto action_name = parser.value("action");
    using namespace std::placeholders;
    if (action_name == "export") {
        action = std::bind(&Operation::to_vault, this, _1, _2, _3);
    } else if (action_name == "import") {
        action = std::bind(&Operation::from_vault, this, _1, _2, _3);
    } else {
        error::raise({{ "msg", "Unknown action"}, {"action", action_name}});
    }

    auto get_home_path = [this](QVariant const &item) {
        map_type res;
        if (hasType(item, QMetaType::QString)) {
            res["path"] = str(item);
        } else if (hasType(item, QMetaType::QVariantMap)) {
            res.unite(item.toMap());
        } else {
            error::raise({{"msg", "Unexpected path type"}, {"item", item}});
        }

        auto path = str(res["path"]);
        if (path.isEmpty())
            error::raise({{"msg", "Invalid data(path)"}, {"item", item}});

        res["full_path"] = os::path::join(home, path);
        res["root_path"] = home;
        return res;
    };

    auto process_home_path = [&action, get_home_path](map_type const &location) {
        for (auto it = location.begin(); it != location.end(); ++it) {
            auto const &name = it.key();
            auto const &items = it.value();
            if (name == "options")
                continue; // skip options
            auto data_type = name;
            auto paths = (hasType(items, QMetaType::QString)
                          ? list_type({get_home_path(items)})
                          : util::map<map_type>(get_home_path, items.toList()));
            action(data_type, box(std::move(paths)), location);
        };
    };

    for (auto it = context.begin(); it != context.end(); ++it) {
        auto const &name = it.key();
        auto const &value = it.value();
        if (name == "options")
            continue; // skip options

        if (name == "home") {
            process_home_path(map(value));
        } else {
            error::raise({{"msg", "Unknown context item"}, {"item", name}});
        }
    };

}

} // namespace

int execute(QVariantMap const &info)
{
    try {
        Operation op(info);
        op.execute();
    } catch (error::Error const &e) {
        qCDebug(lcBackup) << e;
        return 1;
    } catch (std::exception const &e) {
        qCDebug(lcBackup) << e.what();
        return 2;
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
