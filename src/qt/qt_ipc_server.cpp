/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Cross-platform local socket IPC server module (JSON-RPC 2.0).
 *
 * Authors: 86Box Team
 *
 *          Copyright 2026 86Box Team.
 */
#include "qt_ipc_server.hpp"
#include "qt_mainwindow.hpp"
#include "qt_rendererstack.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/keyboard.h>
#include <86box/mouse.h>
#include <86box/machine.h>
#include <86box/video.h>
#ifdef USE_NEW_DYNAREC
#include "../codegen_new/codegen.h"
#include "../codegen_new/codegen_dump.h"
#endif
extern int dopause;
}

// Scancode mapping for standard US layout ASCII input
struct KeyMapEntry {
    uint16_t scan;
    bool     shift;
};

static KeyMapEntry ascii_to_scan(char c)
{
    // Default 0
    if (c >= 'a' && c <= 'z') {
        static const uint16_t az_scans[26] = {
            0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, // a..h
            0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, // i..p
            0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, // q..x
            0x15, 0x2c                                      // y..z
        };
        return { az_scans[c - 'a'], false };
    }
    if (c >= 'A' && c <= 'Z') {
        static const uint16_t az_scans[26] = {
            0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23,
            0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
            0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d,
            0x15, 0x2c
        };
        return { az_scans[c - 'A'], true };
    }
    if (c >= '1' && c <= '9')
        return { static_cast<uint16_t>(0x02 + (c - '1')), false };
    if (c == '0')
        return { 0x0b, false };

    switch (c) {
        case ' ':  return { 0x39, false };
        case '\r':
        case '\n': return { 0x1c, false };
        case '\t': return { 0x0f, false };
        case '\b': return { 0x0e, false };
        case 0x1b: return { 0x01, false }; // Esc
        case '-':  return { 0x0c, false };
        case '=':  return { 0x0d, false };
        case '[':  return { 0x1a, false };
        case ']':  return { 0x1b, false };
        case '\\': return { 0x2b, false };
        case ';':  return { 0x27, false };
        case '\'': return { 0x28, false };
        case '`':  return { 0x29, false };
        case ',':  return { 0x33, false };
        case '.':  return { 0x34, false };
        case '/':  return { 0x35, false };

        case '!':  return { 0x02, true };
        case '@':  return { 0x03, true };
        case '#':  return { 0x04, true };
        case '$':  return { 0x05, true };
        case '%':  return { 0x06, true };
        case '^':  return { 0x07, true };
        case '&':  return { 0x08, true };
        case '*':  return { 0x09, true };
        case '(':  return { 0x0a, true };
        case ')':  return { 0x0b, true };
        case '_':  return { 0x0c, true };
        case '+':  return { 0x0d, true };
        case '{':  return { 0x1a, true };
        case '}':  return { 0x1b, true };
        case '|':  return { 0x2b, true };
        case ':':  return { 0x27, true };
        case '"':  return { 0x28, true };
        case '~':  return { 0x29, true };
        case '<':  return { 0x33, true };
        case '>':  return { 0x34, true };
        case '?':  return { 0x35, true };
        default:   return { 0, false };
    }
}

IPCServer::IPCServer(MainWindow *mainWindow, QObject *parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

IPCServer::~IPCServer()
{
    stop();
}

bool
IPCServer::start(const QString &socketPath)
{
    stop();

    if (socketPath.isEmpty() || socketPath == QStringLiteral("auto")) {
#ifdef Q_OS_WINDOWS
        m_socketPath = QStringLiteral("86box-ipc-%1").arg(QCoreApplication::applicationPid());
#else
        m_socketPath = QStringLiteral("/tmp/86box-ipc-%1.sock").arg(QCoreApplication::applicationPid());
#endif
    } else {
        m_socketPath = socketPath;
    }

    // Clean up any stale socket file
    QLocalServer::removeServer(m_socketPath);

    m_server = new QLocalServer(this);
    connect(m_server, &QLocalServer::newConnection, this, &IPCServer::onNewConnection);

    if (!m_server->listen(m_socketPath)) {
        pclog("IPCServer: failed to listen on '%s': %s\n",
              qUtf8Printable(m_socketPath), qUtf8Printable(m_server->errorString()));
        return false;
    }

#ifndef Q_OS_WINDOWS
    // Create convenient symlink to latest socket
    QString latestPath = QStringLiteral("/tmp/86box-ipc-latest.sock");
    QFile::remove(latestPath);
    QFile::link(m_socketPath, latestPath);
#endif

    pclog("IPCServer: listening on %s (JSON-RPC 2.0)\n", qUtf8Printable(m_socketPath));
    return true;
}

void
IPCServer::stop()
{
    if (m_server) {
        for (auto *client : m_clients) {
            client->disconnect();
            client->close();
            client->deleteLater();
        }
        m_clients.clear();
        m_clientBuffers.clear();

        m_server->close();
        QLocalServer::removeServer(m_socketPath);
        delete m_server;
        m_server = nullptr;
    }
}

void
IPCServer::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket *client = m_server->nextPendingConnection();
        if (!client)
            continue;

        m_clients.append(client);
        m_clientBuffers[client] = QByteArray();

        connect(client, &QLocalSocket::readyRead, this, &IPCServer::onClientReadyRead);
        connect(client, &QLocalSocket::disconnected, this, &IPCServer::onClientDisconnected);
    }
}

void
IPCServer::onClientDisconnected()
{
    auto *client = qobject_cast<QLocalSocket*>(sender());
    if (client) {
        m_clients.removeAll(client);
        m_clientBuffers.remove(client);
        client->deleteLater();
    }
}

void
IPCServer::onClientReadyRead()
{
    auto *client = qobject_cast<QLocalSocket*>(sender());
    if (!client)
        return;

    QByteArray &buffer = m_clientBuffers[client];
    buffer.append(client->readAll());

    int newlineIdx = -1;
    while ((newlineIdx = buffer.indexOf('\n')) != -1) {
        QByteArray line = buffer.left(newlineIdx).trimmed();
        buffer.remove(0, newlineIdx + 1);

        if (!line.isEmpty()) {
            handleMessage(client, line);
        }
    }
}

void
IPCServer::handleMessage(QLocalSocket *client, const QByteArray &line)
{
    QJsonParseError parseError {};
    QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        sendError(client, QJsonValue::Null, -32700, QStringLiteral("Parse error: invalid JSON"));
        return;
    }

    QJsonObject req = doc.object();
    QJsonValue id = req.value(QStringLiteral("id"));
    QString method = req.value(QStringLiteral("method")).toString();
    QJsonObject params = req.value(QStringLiteral("params")).toObject();

    if (method.isEmpty()) {
        sendError(client, id, -32600, QStringLiteral("Invalid Request: missing method"));
        return;
    }

    QString errorMsg;
    QJsonObject result;

    if (method == QStringLiteral("take_screenshot") || method == QStringLiteral("screenshot")) {
        result = handleTakeScreenshot(params, errorMsg);
    } else if (method == QStringLiteral("get_status") || method == QStringLiteral("status")) {
        result = handleGetStatus(params, errorMsg);
    } else if (method == QStringLiteral("pause")) {
        result = handlePause(params, errorMsg);
    } else if (method == QStringLiteral("resume")) {
        result = handleResume(params, errorMsg);
    } else if (method == QStringLiteral("reset")) {
        result = handleReset(params, errorMsg);
    } else if (method == QStringLiteral("ctrl_alt_del") || method == QStringLiteral("cad")) {
        result = handleSendCAD(params, errorMsg);
    } else if (method == QStringLiteral("send_keys") || method == QStringLiteral("type")) {
        result = handleSendKeys(params, errorMsg);
    } else if (method == QStringLiteral("send_mouse") || method == QStringLiteral("mouse")) {
        result = handleSendMouse(params, errorMsg);
    } else if (method == QStringLiteral("stop") || method == QStringLiteral("quit") || method == QStringLiteral("shutdown")) {
        result = handleStop(params, errorMsg);
    } else if (method == QStringLiteral("dynarec_get_stats")) {
        result = handleDynarecGetStats(params, errorMsg);
    } else if (method == QStringLiteral("dynarec_set_link_mask")) {
        result = handleDynarecSetLinkMask(params, errorMsg);
    } else if (method == QStringLiteral("dynarec_flush_cache")) {
        result = handleDynarecFlushCache(params, errorMsg);
    } else if (method == QStringLiteral("dynarec_start_dump")) {
        result = handleDynarecStartDump(params, errorMsg);
    } else if (method == QStringLiteral("dynarec_stop_dump")) {
        result = handleDynarecStopDump(params, errorMsg);
    } else if (method == QStringLiteral("dynarec_arm_trigger")) {
        result = handleDynarecArmTrigger(params, errorMsg);
    } else if (method == QStringLiteral("dynarec_disarm_trigger")) {
        result = handleDynarecDisarmTrigger(params, errorMsg);
    } else {
        sendError(client, id, -32601, QStringLiteral("Method not found: %1").arg(method));
        return;
    }

    if (!errorMsg.isEmpty()) {
        sendError(client, id, -32000, errorMsg);
    } else {
        sendResponse(client, id, result);
    }
}

void
IPCServer::sendResponse(QLocalSocket *client, const QJsonValue &id, const QJsonObject &result)
{
    QJsonObject resp;
    resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    resp[QStringLiteral("id")]      = id;
    resp[QStringLiteral("result")]  = result;

    QByteArray out = QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n";
    client->write(out);
    client->flush();
}

void
IPCServer::sendError(QLocalSocket *client, const QJsonValue &id, int code, const QString &message)
{
    QJsonObject err;
    err[QStringLiteral("code")]    = code;
    err[QStringLiteral("message")] = message;

    QJsonObject resp;
    resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    resp[QStringLiteral("id")]      = id;
    resp[QStringLiteral("error")]   = err;

    QByteArray out = QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n";
    client->write(out);
    client->flush();
}

void
IPCServer::broadcastEvent(const QString &eventName, const QJsonObject &data)
{
    QJsonObject notif;
    notif[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    notif[QStringLiteral("method")]  = eventName;
    notif[QStringLiteral("params")]  = data;

    QByteArray out = QJsonDocument(notif).toJson(QJsonDocument::Compact) + "\n";
    for (auto *client : m_clients) {
        if (client->isOpen()) {
            client->write(out);
            client->flush();
        }
    }
}

// -------------------------------------------------------------------------
// Command Handlers
// -------------------------------------------------------------------------

QJsonObject
IPCServer::handleTakeScreenshot(const QJsonObject &params, QString &errorMsg)
{
    QString path = params.value(QStringLiteral("path")).toString();
    int monitor = params.value(QStringLiteral("monitor")).toInt(0);

    if (path.isEmpty()) {
        QString dir = QString::fromUtf8(usr_path) + QStringLiteral("/screenshots");
        QDir().mkpath(dir);
        QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"));
        path = QStringLiteral("%1/screenshot_%2.png").arg(dir, ts);
    }

    int width = 0, height = 0;
    bool success = m_mainWindow ? m_mainWindow->takeScreenshot(path, monitor, &width, &height) : false;

    if (!success) {
        errorMsg = QStringLiteral("Failed to capture screenshot (renderer buffer unavailable)");
        return {};
    }

    QJsonObject res;
    res[QStringLiteral("path")]   = QFileInfo(path).absoluteFilePath();
    res[QStringLiteral("width")]  = width;
    res[QStringLiteral("height")] = height;
    return res;
}

QJsonObject
IPCServer::handleGetStatus(const QJsonObject &/*params*/, QString &/*errorMsg*/)
{
    QJsonObject res;
    res[QStringLiteral("state")]     = dopause ? QStringLiteral("paused") : QStringLiteral("running");

    char cpu_str[256] = { 0 };
    plat_get_cpu_string(cpu_str, sizeof(cpu_str) - 1);
    res[QStringLiteral("cpu")]       = QString::fromUtf8(cpu_str);
    res[QStringLiteral("machine")]   = QString::fromUtf8(machine_getname(machine));
    res[QStringLiteral("cycles")]    = static_cast<qint64>(cpu_state._cycles);
    res[QStringLiteral("fps")]       = static_cast<qint64>(monitors[0].mon_renderedframes);

#ifdef USE_NEW_DYNAREC
    res[QStringLiteral("is_dynarec")] = true;
    res[QStringLiteral("link_mask")]  = static_cast<int>(codegen_link_mask);
    res[QStringLiteral("dump_active")] = static_cast<bool>(codegen_dump_is_active());
#else
    res[QStringLiteral("is_dynarec")] = false;
#endif

    return res;
}

QJsonObject
IPCServer::handlePause(const QJsonObject &/*params*/, QString &/*errorMsg*/)
{
    plat_pause(1);
    QJsonObject res;
    res[QStringLiteral("state")] = QStringLiteral("paused");
    return res;
}

QJsonObject
IPCServer::handleResume(const QJsonObject &/*params*/, QString &/*errorMsg*/)
{
    plat_pause(0);
    QJsonObject res;
    res[QStringLiteral("state")] = QStringLiteral("running");
    return res;
}

QJsonObject
IPCServer::handleReset(const QJsonObject &params, QString &/*errorMsg*/)
{
    QString type = params.value(QStringLiteral("type")).toString(QStringLiteral("hard"));
    if (m_mainWindow) {
        m_mainWindow->hardReset();
    }
    QJsonObject res;
    res[QStringLiteral("status")] = QStringLiteral("resetting");
    res[QStringLiteral("type")]   = type;
    return res;
}

QJsonObject
IPCServer::handleSendCAD(const QJsonObject &/*params*/, QString &/*errorMsg*/)
{
    pc_send_cad();
    QJsonObject res;
    res[QStringLiteral("status")] = QStringLiteral("ok");
    return res;
}

QJsonObject
IPCServer::handleSendKeys(const QJsonObject &params, QString &errorMsg)
{
    int keysSent = 0;

    // Direct scancodes array
    if (params.contains(QStringLiteral("scancodes"))) {
        QJsonArray scs = params.value(QStringLiteral("scancodes")).toArray();
        for (const auto &val : scs) {
            uint16_t sc = static_cast<uint16_t>(val.toInt());
            keyboard_input(1, sc);
            keyboard_input(0, sc);
            keysSent++;
        }
    } else {
        // Text string
        QString text = params.value(QStringLiteral("string")).toString();
        if (text.isEmpty())
            text = params.value(QStringLiteral("keys")).toString();

        if (text.isEmpty()) {
            errorMsg = QStringLiteral("Missing 'string', 'keys', or 'scancodes' parameter");
            return {};
        }

        for (int i = 0; i < text.length(); ++i) {
            char c = text.at(i).toLatin1();
            KeyMapEntry entry = ascii_to_scan(c);
            if (entry.scan != 0) {
                if (entry.shift)
                    keyboard_input(1, 0x2a); // Shift down

                keyboard_input(1, entry.scan);
                keyboard_input(0, entry.scan);

                if (entry.shift)
                    keyboard_input(0, 0x2a); // Shift up

                keysSent++;
            }
        }
    }

    QJsonObject res;
    res[QStringLiteral("status")]    = QStringLiteral("ok");
    res[QStringLiteral("keys_sent")] = keysSent;
    return res;
}

QJsonObject
IPCServer::handleSendMouse(const QJsonObject &params, QString &/*errorMsg*/)
{
    int dx = params.value(QStringLiteral("dx")).toInt(0);
    int dy = params.value(QStringLiteral("dy")).toInt(0);
    int buttons = params.value(QStringLiteral("buttons")).toInt(0);

    mouse_scale(dx, dy);
    mouse_set_buttons(buttons);
    mouse_process();

    QJsonObject res;
    res[QStringLiteral("status")] = QStringLiteral("ok");
    return res;
}

QJsonObject
IPCServer::handleStop(const QJsonObject &/*params*/, QString &/*errorMsg*/)
{
    if (m_mainWindow) {
        QMetaObject::invokeMethod(m_mainWindow, "close", Qt::QueuedConnection);
    }
    QJsonObject res;
    res[QStringLiteral("status")] = QStringLiteral("stopping");
    return res;
}

QJsonObject
IPCServer::handleDynarecGetStats(const QJsonObject &/*params*/, QString &errorMsg)
{
#ifdef USE_NEW_DYNAREC
    QJsonObject res;
    res[QStringLiteral("link_mask")]      = static_cast<int>(codegen_link_mask);
    res[QStringLiteral("dump_active")]    = static_cast<bool>(codegen_dump_is_active());
    res[QStringLiteral("profile_active")] = static_cast<bool>(codegen_profile_active);
    res[QStringLiteral("hang_detect")]    = static_cast<bool>(codegen_hang_detect_active);
    res[QStringLiteral("trigger_armed")]  = static_cast<bool>(codegen_trigger.armed);
    return res;
#else
    errorMsg = QStringLiteral("Dynarec is not enabled in this build");
    return {};
#endif
}

QJsonObject
IPCServer::handleDynarecSetLinkMask(const QJsonObject &params, QString &errorMsg)
{
#ifdef USE_NEW_DYNAREC
    if (!params.contains(QStringLiteral("mask"))) {
        errorMsg = QStringLiteral("Missing 'mask' parameter");
        return {};
    }
    int was_paused = dopause;
    if (!was_paused)
        plat_pause(1);

    codegen_link_mask = params.value(QStringLiteral("mask")).toInt();

    if (!was_paused)
        plat_pause(0);

    QJsonObject res;
    res[QStringLiteral("link_mask")] = static_cast<int>(codegen_link_mask);
    return res;
#else
    errorMsg = QStringLiteral("Dynarec is not enabled in this build");
    return {};
#endif
}

QJsonObject
IPCServer::handleDynarecFlushCache(const QJsonObject &/*params*/, QString &errorMsg)
{
#ifdef USE_NEW_DYNAREC
    int was_paused = dopause;
    if (!was_paused)
        do_pause(1);

    codegen_reset();

    if (!was_paused)
        do_pause(0);

    QJsonObject res;
    res[QStringLiteral("status")] = QStringLiteral("flushed");
    return res;
#else
    errorMsg = QStringLiteral("Dynarec is not enabled in this build");
    return {};
#endif
}

QJsonObject
IPCServer::handleDynarecStartDump(const QJsonObject &params, QString &errorMsg)
{
#ifdef USE_NEW_DYNAREC
    QString path = params.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        errorMsg = QStringLiteral("Missing 'path' parameter");
        return {};
    }
    quint64 minExec = params.value(QStringLiteral("min_exec")).toVariant().toULongLong();

    int was_paused = dopause;
    if (!was_paused)
        do_pause(1);

    QByteArray pathBytes = path.toUtf8();
    int ok = codegen_dump_start(pathBytes.constData(), minExec);

    if (!was_paused)
        do_pause(0);

    if (!ok) {
        errorMsg = QStringLiteral("Failed to open dump file '%1'").arg(path);
        return {};
    }

    QJsonObject res;
    res[QStringLiteral("status")] = QStringLiteral("dumping");
    res[QStringLiteral("path")]   = QFileInfo(path).absoluteFilePath();
    return res;
#else
    errorMsg = QStringLiteral("Dynarec is not enabled in this build");
    return {};
#endif
}

QJsonObject
IPCServer::handleDynarecStopDump(const QJsonObject &/*params*/, QString &errorMsg)
{
#ifdef USE_NEW_DYNAREC
    int was_paused = dopause;
    if (!was_paused)
        do_pause(1);

    codegen_dump_stop();

    if (!was_paused)
        do_pause(0);

    QJsonObject res;
    res[QStringLiteral("status")] = QStringLiteral("stopped");
    return res;
#else
    errorMsg = QStringLiteral("Dynarec is not enabled in this build");
    return {};
#endif
}

QJsonObject
IPCServer::handleDynarecArmTrigger(const QJsonObject &params, QString &errorMsg)
{
#ifdef USE_NEW_DYNAREC
    QString typeStr = params.value(QStringLiteral("type")).toString().toLower();
    codegen_dump_trigger_t trig {};

    if (typeStr == QStringLiteral("exec")) {
        trig.type = TRIGGER_EXEC_NAME;
        QString pattern = params.value(QStringLiteral("pattern")).toString();
        if (pattern.isEmpty()) {
            errorMsg = QStringLiteral("Missing 'pattern' parameter for exec trigger");
            return {};
        }
        strncpy(trig.pattern, pattern.toUtf8().constData(), sizeof(trig.pattern) - 1);
    } else if (typeStr == QStringLiteral("pc")) {
        trig.type = TRIGGER_PC_RANGE;
        trig.pc_start = params.value(QStringLiteral("pc_start")).toVariant().toUInt();
        trig.pc_end   = params.value(QStringLiteral("pc_end")).toVariant().toUInt();
        if (trig.pc_end == 0)
            trig.pc_end = trig.pc_start;
    } else if (typeStr == QStringLiteral("pattern") || typeStr == QStringLiteral("bytes")) {
        trig.type = TRIGGER_BYTE_PATTERN;
        QString hex = params.value(QStringLiteral("byte_pattern")).toString().remove(' ');
        QByteArray b = QByteArray::fromHex(hex.toLatin1());
        trig.byte_len = qMin(static_cast<uint32_t>(b.size()), static_cast<uint32_t>(32));
        memcpy(trig.byte_pattern, b.constData(), trig.byte_len);
        memset(trig.byte_mask, 0xff, trig.byte_len);
    } else {
        errorMsg = QStringLiteral("Invalid trigger type '%1' (valid: 'exec', 'pc', 'pattern')").arg(typeStr);
        return {};
    }

    QString outPath = params.value(QStringLiteral("output_path")).toString();
    if (!outPath.isEmpty())
        strncpy(trig.output_path, outPath.toUtf8().constData(), sizeof(trig.output_path) - 1);

    trig.max_blocks = params.value(QStringLiteral("max_blocks")).toVariant().toULongLong();
    trig.take_screenshot = params.value(QStringLiteral("take_screenshot")).toBool(false) ? 1 : 0;

    codegen_trigger_arm(&trig);

    QJsonObject res;
    res[QStringLiteral("status")]  = QStringLiteral("armed");
    res[QStringLiteral("type")]    = typeStr;
    res[QStringLiteral("output")]  = QString::fromUtf8(trig.output_path);
    return res;
#else
    errorMsg = QStringLiteral("Dynarec is not enabled in this build");
    return {};
#endif
}

QJsonObject
IPCServer::handleDynarecDisarmTrigger(const QJsonObject &/*params*/, QString &errorMsg)
{
#ifdef USE_NEW_DYNAREC
    codegen_trigger_disarm();
    QJsonObject res;
    res[QStringLiteral("status")] = QStringLiteral("disarmed");
    return res;
#else
    errorMsg = QStringLiteral("Dynarec is not enabled in this build");
    return {};
#endif
}
