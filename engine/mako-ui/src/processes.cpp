/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "processes.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QProcess>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <filesystem>
#include <fstream>
#include <algorithm>

using namespace mako::ui;

QString mako::ui::detectGpuVendor() {
    const std::string drmPath = "/sys/class/drm";
    if (!std::filesystem::exists(drmPath))
        return "unknown";

    for (const auto& entry : std::filesystem::directory_iterator(drmPath)) {
        const auto& path = entry.path();
        auto filename = path.filename().string();
        if (filename.find("card") == std::string::npos)
            continue;

        auto vendorPath = path / "device" / "vendor";
        if (!std::filesystem::exists(vendorPath))
            continue;

        std::ifstream vendorFile(vendorPath);
        std::string vendor;
        std::getline(vendorFile, vendor);

        if (vendor == "0x10de") return "nvidia";
        if (vendor == "0x1002") return "amd";
        if (vendor == "0x8086") return "intel";
    }

    return "unknown";
}

namespace {
    struct RawProcess {
        int pid;
        QString name;
        QString cmdline;
    };

    bool isNumeric(const QString& s) {
        for (const auto& c : s)
            if (!c.isDigit()) return false;
        return !s.isEmpty();
    }

    QString readProcComm(int pid) {
        QFile file(QString("/proc/%1/comm").arg(pid));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        return file.readAll().trimmed();
    }

    // process loads Vulkan libraries
    bool usesVulkan(int pid) {
        QFile mapsFile(QString("/proc/%1/maps").arg(pid));
        if (!mapsFile.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;

        while (!mapsFile.atEnd()) {
            const auto line = mapsFile.readLine();
            if (line.contains("libvulkan") ||
                line.contains("nvidia_icd") ||
                line.contains("radeon_icd") ||
                line.contains("anv_icd") ||
                line.contains("vulkan"))
                return true;
        }
        return false;
    }

    // process holds open GPU device handles (/dev/dri/* or /dev/nvidia*)
    bool hasGpuHandles(int pid) {
        QDir fdDir(QString("/proc/%1/fd").arg(pid));
        if (!fdDir.exists())
            return false;

        const auto entries = fdDir.entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
        for (const auto& entry : entries) {
            const QString target = QFile::symLinkTarget(fdDir.filePath(entry));
            if (target.startsWith("/dev/dri/") || target.startsWith("/dev/nvidia"))
                return true;
        }
        return false;
    }

    // cumulative GPU engine time in nanoseconds from kernel fdinfo
    quint64 getFdinfoGpuTimeNs(int pid) {
        QDir fdinfoDir(QString("/proc/%1/fdinfo").arg(pid));
        if (!fdinfoDir.exists())
            return 0;

        quint64 total = 0;
        const auto entries = fdinfoDir.entryList(QDir::Files | QDir::NoDotAndDotDot);
        for (const auto& entry : entries) {
            QFile file(fdinfoDir.filePath(entry));
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;

            while (!file.atEnd()) {
                const auto line = file.readLine();
                if (!line.startsWith("drm-engine-"))
                    continue;
                const int colon = line.indexOf(':');
                const auto value = QString::fromLatin1(line.mid(colon + 1)).simplified();
                total += value.section(' ', 0, 0).toULongLong();
            }
        }
        return total;
    }

    // sandboxed processes (flatpak, steam, hardened launchers) deny access to
    // /proc/[pid]/fd and maps even for the same uid
    bool isFdDirReadable(int pid) {
        return QFileInfo(QString("/proc/%1/fd").arg(pid)).isReadable();
    }

    // user-launched applications live under the systemd user app slice;
    // readable even for sandboxed processes
    bool inUserAppSlice(int pid) {
        QFile cgroup(QString("/proc/%1/cgroup").arg(pid));
        if (!cgroup.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;

        while (!cgroup.atEnd()) {
            if (cgroup.readLine().contains("/app.slice/"))
                return true;
        }
        return false;
    }

    QList<RawProcess> readAllProcesses() {
        QList<RawProcess> processes;
        QDir procDir("/proc");
        const auto entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

        for (const auto& entry : entries) {
            if (!isNumeric(entry)) continue;
            int pid = entry.toInt();
            if (pid <= 0) continue;

            QFile cmdlineFile(QString("/proc/%1/cmdline").arg(pid));
            if (!cmdlineFile.open(QIODevice::ReadOnly))
                continue;
            QByteArray cmdData = cmdlineFile.readAll();
            if (cmdData.isEmpty()) continue; // kernel thread

            QString name = readProcComm(pid);

            static const QSet<QString> excluded = {
                "mako-ui", "mako-cli",
                "kwin_wayland", "kwin_x11", "Xwayland",
            };
            if (excluded.contains(name)) continue;
            if (name.isEmpty() || name.startsWith("[")) continue;

            QString cmdline = QString::fromLocal8Bit(cmdData).replace('\0', ' ').trimmed();
            processes.append({pid, name, cmdline});
        }

        return processes;
    }

    // per-process GPU utilization via nvidia-smi pmon (sm % column)
    QMap<int, int> getNvidiaGpuUsage() {
        QMap<int, int> usage;

        QProcess pmon;
        pmon.start("nvidia-smi", {"pmon", "-c", "1"});
        pmon.waitForFinished(3000);

        if (pmon.exitCode() == 0 || !pmon.readAllStandardOutput().isEmpty()) {
            const auto lines = QString::fromLocal8Bit(pmon.readAllStandardOutput()).split('\n');
            for (const auto& line : lines) {
                if (line.startsWith('#') || line.trimmed().isEmpty())
                    continue;
                const auto cols = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                if (cols.size() >= 4) {
                    bool ok = false;
                    int pid = cols[1].toInt(&ok);
                    int sm = cols[3].toInt();
                    if (ok && pid > 0 && usage.value(pid, -1) < sm)
                        usage[pid] = sm;
                }
            }
            if (!usage.isEmpty())
                return usage;
        }

        QProcess proc;
        proc.start("nvidia-smi", {
            "--query-compute-apps=pid,used_gpu_memory",
            "--format=csv,noheader,nounits"
        });
        proc.waitForFinished(3000);
        if (proc.exitCode() != 0) return usage;

        const auto lines = QString::fromLocal8Bit(proc.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        for (const auto& line : lines) {
            const auto parts = line.split(',');
            if (parts.size() < 2) continue;
            int pid = parts[0].trimmed().toInt();
            if (pid > 0) usage[pid] = -2; // active on gpu, unknown %
        }

        return usage;
    }

    QMap<int, int> getAmdGpuUsage() {
        QMap<int, int> usage;

        QProcess proc;
        proc.start("rocm-smi", {"--showprocessuse", "--json"});
        proc.waitForFinished(3000);

        if (proc.exitCode() != 0) {
            QProcess fallback;
            fallback.start("rocm-smi", {"--showprocessuse"});
            fallback.waitForFinished(3000);
            if (fallback.exitCode() != 0) return usage;

            const auto output = QString::fromLocal8Bit(fallback.readAllStandardOutput());
            QRegularExpression re(R"(PID\s+(\d+),?\s+(\d+)%?\s*GPU)");
            QRegularExpressionMatchIterator it = re.globalMatch(output);
            while (it.hasNext()) {
                auto match = it.next();
                int pid = match.captured(1).toInt();
                int util = match.captured(2).toInt();
                if (pid > 0) usage[pid] = util;
            }
            return usage;
        }

        const auto json = QJsonDocument::fromJson(proc.readAllStandardOutput()).object();
        const auto processList = json["program-list"].toArray();
        for (const auto& procEntry : processList) {
            auto obj = procEntry.toObject();
            int pid = obj["pid"].toInt();
            int gpuUse = obj["gpu-use"].toInt();
            if (pid > 0) usage[pid] = gpuUse;
        }

        return usage;
    }

    QMap<int, int> getGpuUsageMap() {
        const auto vendor = detectGpuVendor();
        if (vendor == "nvidia") return getNvidiaGpuUsage();
        if (vendor == "amd") return getAmdGpuUsage();
        return {};
    }
}

QList<ProcessInfo> mako::ui::getRunningProcesses() {
    auto rawProcesses = readAllProcesses();
    const auto gpuUsage = getGpuUsageMap();

    QList<ProcessInfo> result;
    for (const auto& rp : rawProcesses) {
        const bool vulkan = usesVulkan(rp.pid);
        bool gpu = false;
        if (!vulkan)
            gpu = hasGpuHandles(rp.pid);

        if (!vulkan && !gpu) {
            // sandboxed user apps deny fd+maps reads; keep them when they are
            // user-launched applications, drop unreadable system processes
            if (isFdDirReadable(rp.pid))
                continue;
            if (!inUserAppSlice(rp.pid))
                continue;
        }

        ProcessInfo info;
        info.pid = rp.pid;
        info.cmdline = rp.cmdline;
        info.gpuUsage = gpuUsage.value(rp.pid, -1);
        info.gpuTimeNs = getFdinfoGpuTimeNs(rp.pid);

        info.name = rp.name;
        if (vulkan)
            info.name += " [Vulkan]";

        result.append(info);
    }

    std::sort(result.begin(), result.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        if (a.gpuUsage >= 0 && b.gpuUsage < 0) return true;
        if (a.gpuUsage < 0 && b.gpuUsage >= 0) return false;
        if (a.gpuUsage != b.gpuUsage) return a.gpuUsage > b.gpuUsage;
        if (a.gpuTimeNs != b.gpuTimeNs) return a.gpuTimeNs > b.gpuTimeNs;
        return a.name.toLower() < b.name.toLower();
    });

    return result;
}
