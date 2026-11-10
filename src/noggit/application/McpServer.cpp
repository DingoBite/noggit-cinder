// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/application/McpServer.hpp>
#include <noggit/Log.h>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/windows/projectSelection/NoggitProjectSelectionWindow.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkProxy>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <exception>

namespace Noggit::Application
{
  namespace
  {
    constexpr int mcp_max_request_size = 8192;

    bool loadMcpConnection(QString& token, int& port)
    {
      if (qEnvironmentVariableIsSet("NOGGIT_MCP_TOKEN"))
      {
        token = qEnvironmentVariable("NOGGIT_MCP_TOKEN");
        port = 38555;
      }
      else
      {
#ifdef Q_OS_WIN
        QString const data_root = qEnvironmentVariable("LOCALAPPDATA");
#else
        QString const data_root = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
#endif
        if (data_root.isEmpty())
        {
          LogError << "MCP user configuration directory is unavailable" << std::endl;
          return false;
        }
        QString const directory = QDir(data_root).filePath("NoggitCinderMCP");
        if (!QDir().mkpath(directory))
        {
          LogError << "MCP could not create its user configuration directory" << std::endl;
          return false;
        }

        QString const path = QDir(directory).filePath("connection.json");
        QLockFile lock(path + ".lock");
        if (!lock.tryLock(5000))
        {
          LogError << "MCP could not lock its user connection configuration" << std::endl;
          return false;
        }

        QFile existing(path);
        QJsonObject connection;
        if (existing.exists())
        {
          if (!existing.open(QIODevice::ReadOnly) || existing.size() > 4096)
          {
            LogError << "MCP could not read its user connection configuration" << std::endl;
            return false;
          }
          auto const document = QJsonDocument::fromJson(existing.readAll());
          if (!document.isObject())
          {
            LogError << "MCP user connection configuration must be a JSON object" << std::endl;
            return false;
          }
          connection = document.object();
        }
        else
        {
          QString generated_token;
          for (int i = 0; i < 8; ++i)
            generated_token += QString("%1").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
          connection = {{"token", generated_token}, {"port", 38555}};
          auto const data = QJsonDocument(connection).toJson(QJsonDocument::Compact);
          QSaveFile output(path);
          if (!output.open(QIODevice::WriteOnly)
              || !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
              || output.write(data) != data.size() || !output.commit())
          {
            LogError << "MCP could not save its user connection configuration" << std::endl;
            return false;
          }
        }

        token = connection.value("token").toString();
        auto const stored_port = connection.value("port");
        port = stored_port.isDouble() ? stored_port.toInt(-1) : -1;
      }

      QRegularExpression const token_format("\\A[0-9a-fA-F]{32,128}\\z");
      if (!token_format.match(token).hasMatch())
      {
        LogError << "MCP token must contain 32 to 128 hexadecimal digits" << std::endl;
        return false;
      }

      bool valid_port = true;
      if (qEnvironmentVariableIsSet("NOGGIT_MCP_PORT"))
        port = qEnvironmentVariable("NOGGIT_MCP_PORT").toInt(&valid_port);
      if (!valid_port || port < 1 || port > 65535)
      {
        LogError << "MCP port must be an integer from 1 to 65535" << std::endl;
        return false;
      }
      return true;
    }

    QJsonObject dispatchMcpRequest(QByteArray const& line, QString const& token)
    {
      auto const document = QJsonDocument::fromJson(line);
      if (!document.isObject())
        return {{"error", "Expected a JSON object"}};

      auto const request = document.object();
      if (request.value("token").toString() != token)
        return {{"error", "Unauthorized"}};

      auto const method = request.value("method").toString();
      auto const arguments = request.value("arguments");
      if (!arguments.isObject())
        return {{"error", "Expected an arguments object"}};

      for (auto* widget : QApplication::topLevelWidgets())
      {
        auto* editor = qobject_cast<Ui::Windows::NoggitWindow*>(widget);
        if (editor && editor->isVisible())
        {
          if (method == "open_project")
            return {{"error", "A Noggit project is already open"}};
          return editor->handleMcpRequest(method, arguments.toObject());
        }
      }

      if (method == "open_project")
      {
        for (auto* widget : QApplication::topLevelWidgets())
        {
          auto* selector = qobject_cast<Ui::Windows::NoggitProjectSelectionWindow*>(widget);
          if (selector && selector->isVisible())
            return selector->openMcpProject(arguments.toObject().value("project_path").toString());
        }
        return {{"error", "The Noggit project selection window is unavailable"}};
      }

      if (method == "status")
        return {{"result", QJsonObject{{"state", "project_selection"}}}};
      return {{"error", "Open a Noggit project first"}};
    }

    void replyToMcpRequest(QTcpSocket* socket, QTimer* read_deadline, QString const& token)
    {
      if (!read_deadline->isActive())
        return;
      bool const oversized = socket->bytesAvailable() > mcp_max_request_size;
      if (!oversized && !socket->canReadLine())
        return;

      read_deadline->stop();
      QPointer<QTcpSocket> const connected_socket(socket);
      QJsonObject response;
      try
      {
        response = oversized
          ? QJsonObject{{"error", "Request exceeds 8192 bytes"}}
          : dispatchMcpRequest(socket->readLine(), token);
      }
      catch (std::exception const& error)
      {
        LogError << "MCP command failed: " << error.what() << std::endl;
        response = {{"error", "Editor command failed. See the Noggit log."}};
      }
      catch (...)
      {
        LogError << "MCP command failed with an unknown exception" << std::endl;
        response = {{"error", "Editor command failed. See the Noggit log."}};
      }

      if (connected_socket)
      {
        connected_socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
        connected_socket->disconnectFromHost();
      }
    }
  }

  void startMcpServer()
  {
    QString token;
    int port = 38555;
    if (!loadMcpConnection(token, port))
      return;

    auto* server = new QTcpServer(QCoreApplication::instance());
    server->setProxy(QNetworkProxy::NoProxy);
    if (!server->listen(QHostAddress::LocalHost, static_cast<quint16>(port)))
    {
      LogError << "MCP could not listen on 127.0.0.1:" << port
               << ": " << server->errorString().toStdString() << std::endl;
      delete server;
      return;
    }

    QObject::connect(server, &QTcpServer::newConnection, server, [server, token]
    {
      while (auto* socket = server->nextPendingConnection())
      {
        socket->setReadBufferSize(mcp_max_request_size + 1);
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        auto* read_deadline = new QTimer(socket);
        read_deadline->setSingleShot(true);
        QObject::connect(read_deadline, &QTimer::timeout, socket, &QTcpSocket::abort);
        read_deadline->start(5000);
        auto const receive_request = [socket, read_deadline, token]
        {
          replyToMcpRequest(socket, read_deadline, token);
        };
        QObject::connect(socket, &QTcpSocket::readyRead, socket, receive_request);
        receive_request();
      }
    });

    Log << "MCP editor bridge listening on 127.0.0.1:" << port << std::endl;
  }
}
