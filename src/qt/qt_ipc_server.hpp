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
#ifndef QT_IPC_SERVER_HPP
#define QT_IPC_SERVER_HPP

#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QString>

class MainWindow;

class IPCServer final : public QObject {
    Q_OBJECT

public:
    explicit IPCServer(MainWindow *mainWindow, QObject *parent = nullptr);
    ~IPCServer() override;

    bool start(const QString &socketPath = QString());
    void stop();
    QString socketPath() const { return m_socketPath; }

public slots:
    void broadcastEvent(const QString &eventName, const QJsonObject &data);

private slots:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();

private:
    void handleMessage(QLocalSocket *client, const QByteArray &line);
    void sendResponse(QLocalSocket *client, const QJsonValue &id, const QJsonObject &result);
    void sendError(QLocalSocket *client, const QJsonValue &id, int code, const QString &message);

    // Command Handlers
    QJsonObject handleTakeScreenshot(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleGetStatus(const QJsonObject &params, QString &errorMsg);
    QJsonObject handlePause(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleResume(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleReset(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleSendCAD(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleSendKeys(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleSendMouse(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleStop(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleDynarecGetStats(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleDynarecSetLinkMask(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleDynarecFlushCache(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleDynarecStartDump(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleDynarecStopDump(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleDynarecArmTrigger(const QJsonObject &params, QString &errorMsg);
    QJsonObject handleDynarecDisarmTrigger(const QJsonObject &params, QString &errorMsg);

    MainWindow *m_mainWindow = nullptr;
    QLocalServer *m_server = nullptr;
    QString m_socketPath;
    QList<QLocalSocket*> m_clients;
    QMap<QLocalSocket*, QByteArray> m_clientBuffers;
};

#endif // QT_IPC_SERVER_HPP
