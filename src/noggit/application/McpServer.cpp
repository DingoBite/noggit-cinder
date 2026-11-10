// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/application/McpServer.hpp>
#include <noggit/Log.h>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <exception>

namespace Noggit::Application
{
  namespace
  {
    constexpr int mcp_max_request_size = 8192;

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
          return editor->handleMcpRequest(method, arguments.toObject());
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
    QString const token = qEnvironmentVariable("NOGGIT_MCP_TOKEN");
    if (token.isEmpty())
      return;
    QRegularExpression const token_format("\\A[0-9a-fA-F]{32,128}\\z");
    if (!token_format.match(token).hasMatch())
    {
      LogError << "NOGGIT_MCP_TOKEN must contain 32 to 128 hexadecimal digits" << std::endl;
      return;
    }

    bool valid_port = true;
    int const port = qEnvironmentVariableIsSet("NOGGIT_MCP_PORT")
      ? qEnvironmentVariable("NOGGIT_MCP_PORT").toInt(&valid_port)
      : 38555;
    if (!valid_port || port < 1 || port > 65535)
    {
      LogError << "NOGGIT_MCP_PORT must be an integer from 1 to 65535" << std::endl;
      return;
    }

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
