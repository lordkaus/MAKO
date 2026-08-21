/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "processes.hpp"

#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>

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

    QString readProcComm(int pid) {
        QFile file(QString("/proc/%1/comm").arg(pid));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        return file.readAll().trimmed();
    }

    bool isNumeric(const QString& s) {
        for (const auto& c : s)
            if (!c.isDigit()) return false;
        return !s.isEmpty();
    }

    // get PIDs of processes that have a visible X11/Wayland window
    QSet<int> getWindowPids() {
        QSet<int> pids;

        // try wmctrl first
        QProcess wmctrl;
        wmctrl.start("wmctrl", {"-l", "-p"});
        wmctrl.waitForFinished(3000);
        if (wmctrl.exitCode() == 0) {
            const auto lines = QString::fromLocal8Bit(wmctrl.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
            for (const auto& line : lines) {
                const auto parts = line.split(QRegularExpression("\\s+"));
                if (parts.size() >= 3) {
                    bool ok;
                    int pid = parts[2].toInt(&ok);
                    if (ok && pid > 0)
                        pids.insert(pid);
                }
            }
            if (!pids.isEmpty())
                return pids;
        }

        // fallback: xdotool
        QProcess xdotool;
        xdotool.start("xdotool", {"search", "--onlyvisible", "--name", ""});
        xdotool.waitForFinished(3000);
        if (xdotool.exitCode() == 0) {
            const auto lines = QString::fromLocal8Bit(xdotool.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
            for (const auto& wid : lines) {
                QProcess pidQuery;
                pidQuery.start("xdotool", {"getwindowpid", wid.trimmed()});
                pidQuery.waitForFinished(1000);
                if (pidQuery.exitCode() == 0) {
                    bool ok;
                    int pid = QString::fromLocal8Bit(pidQuery.readAllStandardOutput()).trimmed().toInt(&ok);
                    if (ok && pid > 0)
                        pids.insert(pid);
                }
            }
        }

        return pids;
    }

    // check if a process uses Vulkan by reading /proc/[pid]/maps
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

    // get window title for a PID (from WM_CLASS or _NET_WM_NAME)
    QString getWindowTitle(int pid) {
        // try xdotool to get window name
        QProcess xdotool;
        xdotool.start("xdotool", {"search", "--pid", QString::number(pid), "--name", "getwindowname"});
        xdotool.waitForFinished(2000);
        if (xdotool.exitCode() == 0) {
            auto name = QString::fromLocal8Bit(xdotool.readAllStandardOutput()).trimmed();
            if (!name.isEmpty())
                return name;
        }
        return {};
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
            if (cmdData.isEmpty()) continue;

            QString name = readProcComm(pid);
            QString cmdline = QString::fromLocal8Bit(cmdData).replace('\0', ' ').trimmed();

            if (name == "mako-ui" || name == "mako-cli") continue;
            if (name.isEmpty() || name.startsWith("[")) continue;

            processes.append({pid, name, cmdline});
        }

        return processes;
    }

    QMap<int, int> getNvidiaGpuUsage() {
        QMap<int, int> usage;

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
            if (pid > 0) usage[pid] = -2;
        }

        QProcess utilProc;
        utilProc.start("nvidia-smi", {
            "--query-gpu=utilization.gpu",
            "--format=csv,noheader,nounits"
        });
        utilProc.waitForFinished(3000);

        if (utilProc.exitCode() == 0) {
            int gpuUtil = QString::fromLocal8Bit(utilProc.readAllStandardOutput()).trimmed().toInt();
            for (auto it = usage.begin(); it != usage.end(); ++it) {
                if (it.value() == -2) it.value() = gpuUtil;
            }
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
    const auto windowPids = getWindowPids();

    QList<ProcessInfo> result;
    for (const auto& rp : rawProcesses) {
        // only show processes with a visible window
        if (!windowPids.contains(rp.pid))
            continue;

        ProcessInfo info;
        info.pid = rp.pid;
        info.name = rp.name;
        info.cmdline = rp.cmdline;
        info.gpuUsage = gpuUsage.value(rp.pid, -1);

        // check if uses Vulkan
        bool vulkan = usesVulkan(rp.pid);
        QString displayName = rp.name;

        // get window title if available
        QString winTitle = getWindowTitle(rp.pid);
        if (!winTitle.isEmpty() && winTitle != displayName)
            displayName = winTitle + " (" + rp.name + ")";

        if (vulkan)
            displayName += " [Vulkan]";

        info.name = displayName;
        result.append(info);
    }

    // sort: highest GPU usage first
    std::sort(result.begin(), result.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        if (a.gpuUsage >= 0 && b.gpuUsage < 0) return true;
        if (a.gpuUsage < 0 && b.gpuUsage >= 0) return false;
        if (a.gpuUsage >= 0 && b.gpuUsage >= 0) return a.gpuUsage > b.gpuUsage;
        return a.name.toLower() < b.name.toLower();
    });

    return result;
}
