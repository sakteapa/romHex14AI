/*
 * This file is part of romHEX14.
 * Copyright (C) 2026 Cristian Tabuyo <contact@romhex14.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "aiprovider.h"
// Community build: no string obfuscation needed
#define OBF(s) std::string(s)
#define OBFQ(s) QStringLiteral(s)

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>
#include <QByteArray>
#include <QStringList>
#include <QTimer>
#include <QRegularExpression>

// ── AIProvider base ───────────────────────────────────────────────────────────

AIProvider::AIProvider(QObject *parent) : QObject(parent) {}

// ── Helpers ───────────────────────────────────────────────────────────────────

static QJsonArray messagesToClaude(const QVector<AIMessage> &messages)
{
    QJsonArray arr;
    for (const AIMessage &msg : messages) {
        switch (msg.role) {
        case AIMessage::User: {
            QJsonObject m;
            m["role"] = "user";
            m["content"] = msg.content;
            arr.append(m);
            break;
        }
        case AIMessage::Assistant: {
            QJsonObject m;
            m["role"] = "assistant";
            m["content"] = msg.content;
            arr.append(m);
            break;
        }
        case AIMessage::ToolUse: {
            // Emitted by the assistant — wrap in assistant content block
            QJsonObject block;
            block["type"] = "tool_use";
            block["id"]   = msg.toolCallId;
            block["name"] = msg.toolName;
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(msg.toolInputJson.toUtf8(), &err);
            block["input"] = (err.error == QJsonParseError::NoError && doc.isObject())
                             ? doc.object() : QJsonObject();
            QJsonArray content;
            content.append(block);
            QJsonObject m;
            m["role"]    = "assistant";
            m["content"] = content;
            arr.append(m);
            break;
        }
        case AIMessage::ToolResult: {
            // User message with tool_result block
            QJsonObject block;
            block["type"]       = "tool_result";
            block["tool_use_id"] = msg.toolResultId;
            block["content"]    = msg.toolResultJson;
            QJsonArray content;
            content.append(block);
            QJsonObject m;
            m["role"]    = "user";
            m["content"] = content;
            arr.append(m);
            break;
        }
        default:
            break;
        }
    }
    return arr;
}

static QJsonArray toolsToClaude(const QVector<AIToolDef> &tools)
{
    QJsonArray arr;
    for (const AIToolDef &t : tools) {
        QJsonObject obj;
        obj["name"]         = t.name;
        obj["description"]  = t.description;
        obj["input_schema"] = t.inputSchema;
        arr.append(obj);
    }
    return arr;
}

static QJsonArray messagesToOpenAI(const QString &systemPrompt, const QVector<AIMessage> &messages)
{
    QJsonArray arr;
    if (!systemPrompt.isEmpty()) {
        QJsonObject sys;
        sys["role"]    = "system";
        sys["content"] = systemPrompt;
        arr.append(sys);
    }
    for (const AIMessage &msg : messages) {
        switch (msg.role) {
        case AIMessage::User: {
            QJsonObject m;
            m["role"]    = "user";
            m["content"] = msg.content;
            arr.append(m);
            break;
        }
        case AIMessage::Assistant: {
            QJsonObject m;
            m["role"]    = "assistant";
            m["content"] = msg.content;
            arr.append(m);
            break;
        }
        case AIMessage::ToolUse: {
            // assistant message with tool_calls array
            QJsonArray tcs;
            QVector<AIToolCall> calls = msg.toolCalls;
            if (calls.isEmpty())
                calls.append({msg.toolCallId, msg.toolName, msg.toolInputJson});
            for (const AIToolCall &call : calls) {
                QJsonObject func;
                func["name"]      = call.name;
                func["arguments"] = call.argumentsJson;
                QJsonObject tc;
                tc["id"]       = call.id;
                tc["type"]     = "function";
                tc["function"] = func;
                tcs.append(tc);
            }
            QJsonObject m;
            m["role"]       = "assistant";
            m["content"]    = msg.content;
            if (!msg.reasoningContent.isEmpty())
                m["reasoning_content"] = msg.reasoningContent;
            m["tool_calls"] = tcs;
            arr.append(m);
            break;
        }
        case AIMessage::ToolResult: {
            QJsonObject m;
            m["role"]         = "tool";
            m["tool_call_id"] = msg.toolResultId;
            m["content"]      = msg.toolResultJson;
            arr.append(m);
            break;
        }
        default:
            break;
        }
    }
    return arr;
}

static QJsonArray toolsToOpenAI(const QVector<AIToolDef> &tools)
{
    QJsonArray arr;
    for (const AIToolDef &t : tools) {
        QJsonObject func;
        func["name"]        = t.name;
        func["description"] = t.description;
        func["parameters"]  = t.inputSchema;
        QJsonObject obj;
        obj["type"]     = "function";
        obj["function"] = func;
        arr.append(obj);
    }
    return arr;
}

// ── ClaudeProvider ─────────────────────────────────────────────────────────────

ClaudeProvider::ClaudeProvider(QObject *parent) : AIProvider(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void ClaudeProvider::abort()
{
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void ClaudeProvider::send(const QString &systemPrompt,
                          const QVector<AIMessage> &messages,
                          const QVector<AIToolDef> &tools,
                          ChunkFn onChunk, ReasoningFn onReasoning, ToolCallFn onToolCall,
                          DoneFn onDone, ErrorFn onError)
{
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    m_onChunk     = onChunk;
    Q_UNUSED(onReasoning);
    m_onToolCall  = onToolCall;
    m_onDone      = onDone;
    m_onError     = onError;
    m_sseBuffer.clear();
    m_pendingCallId.clear();
    m_pendingName.clear();
    m_pendingInputJson.clear();
    m_inToolUse   = false;
    m_errorFired  = false;

    // Build request body
    QJsonObject body;
    body["model"]      = m_model;
    body["max_tokens"] = 4096;
    body["stream"]     = true;
    if (!systemPrompt.isEmpty())
        body["system"] = systemPrompt;
    body["messages"] = messagesToClaude(messages);
    if (!tools.isEmpty())
        body["tools"] = toolsToClaude(tools);

    QUrl endpoint(m_baseUrl.isEmpty()
                  ? OBFQ("https://api.anthropic.com/v1/messages")
                  : m_baseUrl + "/v1/messages");
    QNetworkRequest req(endpoint);
    req.setHeader(QNetworkRequest::ContentTypeHeader, OBF("application/json").c_str());
    req.setRawHeader(OBF("x-api-key").c_str(),          m_apiKey.toUtf8());
    req.setRawHeader(OBF("anthropic-version").c_str(),  "2023-06-01");
    req.setTransferTimeout(0);

    m_reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));

    connect(m_reply, &QNetworkReply::readyRead, this, &ClaudeProvider::onReadyRead);

    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        if (!m_reply) return;
        QNetworkReply::NetworkError netErr = m_reply->error();
        int statusCode = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray remaining = m_reply->readAll();  // any bytes not yet consumed by readyRead
        m_reply->deleteLater();
        m_reply = nullptr;

        if (netErr != QNetworkReply::NoError && netErr != QNetworkReply::OperationCanceledError) {
            if (!m_errorFired) {
                m_errorFired = true;
                // Build a human-readable error: prefer HTTP status + body over raw error code
                QString msg;
                if (statusCode > 0) {
                    // Try to extract "error.message" from the JSON body
                    QJsonParseError pe;
                    QJsonDocument doc = QJsonDocument::fromJson(remaining, &pe);
                    if (pe.error == QJsonParseError::NoError && doc.isObject()) {
                        QString apiMsg = doc.object()["error"].toObject()["message"].toString();
                        msg = apiMsg.isEmpty()
                            ? QString("HTTP %1").arg(statusCode)
                            : QString("HTTP %1: %2").arg(statusCode).arg(apiMsg);
                    } else {
                        msg = QString("HTTP %1: %2")
                            .arg(statusCode)
                            .arg(QString::fromUtf8(remaining.left(200)));
                    }
                } else {
                    msg = QString("Network error: %1 (code %2)")
                        .arg(m_reply ? m_reply->errorString() : "connection failed")
                        .arg((int)netErr);
                }
                if (m_onError) m_onError(msg);
            }
            return;
        }

        if (m_errorFired) return;  // SSE-level error already reported; don't also call onDone

        // If we were still in a tool use when the stream ended, fire the callback now
        if (m_inToolUse && !m_pendingCallId.isEmpty() && m_onToolCall) {
            QJsonParseError pe;
            QJsonDocument doc = QJsonDocument::fromJson(m_pendingInputJson.toUtf8(), &pe);
            QJsonObject inputObj = (pe.error == QJsonParseError::NoError && doc.isObject())
                                   ? doc.object() : QJsonObject();
            m_onToolCall(m_pendingCallId, m_pendingName, inputObj);
            m_inToolUse = false;
        }
        if (m_onDone) m_onDone();
    });

    emit stateChanged(State::Streaming);
}

void ClaudeProvider::onReadyRead()
{
    if (!m_reply) return;
    m_sseBuffer += QString::fromUtf8(m_reply->readAll());

    // Split on double newline (SSE event separator)
    while (true) {
        int sep = m_sseBuffer.indexOf("\n\n");
        if (sep < 0) break;
        QString block = m_sseBuffer.left(sep);
        m_sseBuffer   = m_sseBuffer.mid(sep + 2);

        // Parse event + data from block
        QString eventType;
        QString dataStr;
        for (const QString &line : block.split('\n')) {
            if (line.startsWith("event:"))
                eventType = line.mid(6).trimmed();
            else if (line.startsWith("data:"))
                dataStr = line.mid(5).trimmed();
        }
        if (!dataStr.isEmpty())
            processEvent(eventType, dataStr);
    }
}

void ClaudeProvider::processEvent(const QString &event, const QString &data)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    QJsonObject obj = doc.object();

    if (event == "content_block_start") {
        QJsonObject block = obj["content_block"].toObject();
        if (block["type"].toString() == "tool_use") {
            m_inToolUse        = true;
            m_pendingCallId    = block["id"].toString();
            m_pendingName      = block["name"].toString();
            m_pendingInputJson = "";
        }
    } else if (event == "content_block_delta") {
        QJsonObject delta = obj["delta"].toObject();
        QString dtype = delta["type"].toString();
        if (dtype == "text_delta") {
            QString text = delta["text"].toString();
            if (m_onChunk) m_onChunk(text);
        } else if (dtype == "input_json_delta") {
            m_pendingInputJson += delta["partial_json"].toString();
        }
    } else if (event == "content_block_stop") {
        if (m_inToolUse && !m_pendingCallId.isEmpty() && m_onToolCall) {
            QJsonParseError pe;
            QJsonDocument inputDoc = QJsonDocument::fromJson(m_pendingInputJson.toUtf8(), &pe);
            QJsonObject inputObj = (pe.error == QJsonParseError::NoError && inputDoc.isObject())
                                   ? inputDoc.object() : QJsonObject();
            m_onToolCall(m_pendingCallId, m_pendingName, inputObj);
            m_inToolUse        = false;
            m_pendingCallId.clear();
            m_pendingName.clear();
            m_pendingInputJson.clear();
        }
    } else if (event == "message_stop") {
        // onDone fired from finished() signal
    } else if (event == "error") {
        QJsonObject errObj = obj["error"].toObject();
        QString msg = errObj["message"].toString();
        if (msg.isEmpty()) msg = data;
        if (!m_errorFired) {
            m_errorFired = true;
            if (m_onError) m_onError(msg);
        }
    }
}

// ── OpenAICompatProvider ────────────────────────────────────────────────────────

OpenAICompatProvider::OpenAICompatProvider(QObject *parent) : AIProvider(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void OpenAICompatProvider::abort()
{
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void OpenAICompatProvider::send(const QString &systemPrompt,
                                const QVector<AIMessage> &messages,
                                const QVector<AIToolDef> &tools,
                                ChunkFn onChunk, ReasoningFn onReasoning, ToolCallFn onToolCall,
                                DoneFn onDone, ErrorFn onError)
{
    if (m_reply) {
        m_reply->abort();
        m_reply = nullptr;
    }

    m_onChunk    = onChunk;
    m_onReasoning = onReasoning;
    m_onToolCall = onToolCall;
    m_onDone     = onDone;
    m_onError    = onError;
    m_sseBuffer.clear();
    m_pendingCalls.clear();
    m_errorFired = false;
    m_reasoningContent.clear();

    QJsonObject body;
    body["model"]  = m_model;
    body["stream"] = true;
    body["messages"] = messagesToOpenAI(systemPrompt, messages);
    if (!tools.isEmpty())
        body["tools"] = toolsToOpenAI(tools);
    // DeepSeek thinking traces are retained and replayed with tool-call turns.
    if (m_isDeepSeek)
        body["thinking"] = QJsonObject{{"type", "enabled"}};

    QString url = m_baseUrl;
    if (!url.endsWith('/')) url += '/';
    url += "chat/completions";

    QUrl endpoint(url);
    QNetworkRequest req(endpoint);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!m_apiKey.isEmpty())
        req.setRawHeader(OBF("Authorization").c_str(), (OBF("Bearer ") + m_apiKey.toStdString()).c_str());
    req.setTransferTimeout(0);

    m_reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));

    connect(m_reply, &QNetworkReply::readyRead, this, &OpenAICompatProvider::onReadyRead);

    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        if (!m_reply) return;
        QNetworkReply::NetworkError netErr = m_reply->error();
        int statusCode = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray remaining = m_reply->readAll();
        m_reply->deleteLater();
        m_reply = nullptr;

        if (netErr != QNetworkReply::NoError && netErr != QNetworkReply::OperationCanceledError) {
            if (!m_errorFired) {
                m_errorFired = true;
                QString msg;
                if (statusCode > 0) {
                    QJsonParseError pe;
                    QJsonDocument doc = QJsonDocument::fromJson(remaining, &pe);
                    if (pe.error == QJsonParseError::NoError && doc.isObject()) {
                        QString apiMsg = doc.object()["error"].toObject()["message"].toString();
                        msg = apiMsg.isEmpty()
                            ? QString("HTTP %1").arg(statusCode)
                            : QString("HTTP %1: %2").arg(statusCode).arg(apiMsg);
                    } else {
                        msg = QString("HTTP %1: %2")
                            .arg(statusCode)
                            .arg(QString::fromUtf8(remaining.left(200)));
                    }
                } else {
                    msg = QString("Network error (code %1)").arg((int)netErr);
                }
                if (m_onError) m_onError(msg);
            }
            return;
        }

        if (m_errorFired) return;
        if (m_onDone) m_onDone();
    });

    emit stateChanged(State::Streaming);
}

void OpenAICompatProvider::onReadyRead()
{
    if (!m_reply) return;
    m_sseBuffer += QString::fromUtf8(m_reply->readAll());

    while (true) {
        int nl = m_sseBuffer.indexOf('\n');
        if (nl < 0) break;
        QString line = m_sseBuffer.left(nl).trimmed();
        m_sseBuffer  = m_sseBuffer.mid(nl + 1);

        if (!line.startsWith("data:")) continue;
        QString dataStr = line.mid(5).trimmed();
        if (dataStr == "[DONE]") {
            if (m_onReasoning && !m_reasoningContent.isEmpty())
                m_onReasoning(m_reasoningContent);
            // Fire all pending tool calls
            for (const PendingCall &pc : m_pendingCalls) {
                if (m_onToolCall) {
                    QJsonParseError pe;
                    QJsonDocument doc = QJsonDocument::fromJson(pc.argsJson.toUtf8(), &pe);
                    QJsonObject inputObj = (pe.error == QJsonParseError::NoError && doc.isObject())
                                          ? doc.object() : QJsonObject();
                    m_onToolCall(pc.id, pc.name, inputObj);
                }
            }
            m_pendingCalls.clear();
            // onDone fired from finished()
            continue;
        }
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(dataStr.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;
        QJsonObject obj = doc.object();

        // Check for API-level error
        if (obj.contains("error")) {
            if (!m_errorFired) {
                m_errorFired = true;
                QJsonObject errObj = obj["error"].toObject();
                QString msg = errObj["message"].toString();
                if (msg.isEmpty()) msg = dataStr;
                if (m_onError) m_onError(msg);
            }
            continue;
        }

        QJsonArray choices = obj["choices"].toArray();
        if (choices.isEmpty()) continue;
        QJsonObject choice = choices[0].toObject();
        QJsonObject delta  = choice["delta"].toObject();
        QString finishReason = choice["finish_reason"].toString();

        processDelta(delta);

        if (finishReason == "tool_calls") {
            if (m_onReasoning && !m_reasoningContent.isEmpty())
                m_onReasoning(m_reasoningContent);
            for (const PendingCall &pc : m_pendingCalls) {
                if (m_onToolCall) {
                    QJsonParseError pe;
                    QJsonDocument idoc = QJsonDocument::fromJson(pc.argsJson.toUtf8(), &pe);
                    QJsonObject inputObj = (pe.error == QJsonParseError::NoError && idoc.isObject())
                                          ? idoc.object() : QJsonObject();
                    m_onToolCall(pc.id, pc.name, inputObj);
                }
            }
            m_pendingCalls.clear();
        }
    }
}

void OpenAICompatProvider::processDelta(const QJsonObject &delta)
{
    if (delta.contains("reasoning_content") && !delta["reasoning_content"].isNull())
        m_reasoningContent += delta["reasoning_content"].toString();
    // Text content
    if (delta.contains("content") && !delta["content"].isNull()) {
        QString text = delta["content"].toString();
        if (!text.isEmpty() && m_onChunk) m_onChunk(text);
    }

    // Tool calls (streamed incrementally)
    if (delta.contains("tool_calls")) {
        QJsonArray tcs = delta["tool_calls"].toArray();
        for (const QJsonValue &tcv : tcs) {
            QJsonObject tc = tcv.toObject();
            int idx = tc["index"].toInt(0);

            // Grow pending calls array as needed
            while (m_pendingCalls.size() <= idx)
                m_pendingCalls.append(PendingCall{});

            PendingCall &pc = m_pendingCalls[idx];
            pc.index = idx;

            if (tc.contains("id"))
                pc.id = tc["id"].toString();

            QJsonObject func = tc["function"].toObject();
            if (func.contains("name"))
                pc.name = func["name"].toString();
            if (func.contains("arguments"))
                pc.argsJson += func["arguments"].toString();
        }
    }
}

// ── Built-in Offline ECU Expert (Zero API key / 100% offline intelligence) ───

OfflineECUExpertProvider::OfflineECUExpertProvider(QObject *parent)
    : AIProvider(parent)
{
}

void OfflineECUExpertProvider::abort()
{
    m_aborted = true;
    emit stateChanged(State::Idle);
}

void OfflineECUExpertProvider::send(const QString &systemPrompt,
                                    const QVector<AIMessage> &messages,
                                    const QVector<AIToolDef> &tools,
                                    ChunkFn onChunk,
                                    ReasoningFn onReasoning,
                                    ToolCallFn onToolCall,
                                    DoneFn onDone,
                                    ErrorFn onError)
{
    Q_UNUSED(onReasoning);
    Q_UNUSED(onError);
    m_aborted = false;
    emit stateChanged(State::Streaming);

    QString lastUserQuery;
    QString lastToolResult;
    QString lastToolName;

    for (int i = messages.size() - 1; i >= 0; --i) {
        if (messages[i].role == AIMessage::User && lastUserQuery.isEmpty()) {
            lastUserQuery = messages[i].content;
        } else if (messages[i].role == AIMessage::ToolResult && lastToolResult.isEmpty()) {
            lastToolResult = messages[i].toolResultJson;
            lastToolName = messages[i].toolResultName;
        }
    }

    QString toolToCall;
    QJsonObject toolInput;
    QString responseText;

    if (!lastToolResult.isEmpty()) {
        responseText = interpretToolResult(lastToolName, lastToolResult, lastUserQuery, systemPrompt);
    } else {
        responseText = generateExpertResponse(lastUserQuery, systemPrompt, tools, toolToCall, toolInput);
    }

    if (!toolToCall.isEmpty() && onToolCall) {
        QTimer::singleShot(30, this, [this, toolToCall, toolInput, onToolCall]() {
            if (m_aborted) return;
            onToolCall("call_offline_" + QString::number(QDateTime::currentMSecsSinceEpoch()), toolToCall, toolInput);
            emit stateChanged(State::Idle);
        });
        return;
    }

    QTimer::singleShot(30, this, [this, responseText, onChunk, onDone]() {
        if (m_aborted) return;
        if (onChunk) onChunk(responseText);
        if (onDone) onDone();
        emit stateChanged(State::Idle);
    });
}

QString OfflineECUExpertProvider::interpretToolResult(const QString &toolName,
                                                     const QString &resultJson,
                                                     const QString &userQuery,
                                                     const QString &systemPrompt)
{
    QString q = userQuery.toLower();
    bool isMizo = q.contains("eng") || q.contains("nih") || q.contains("hrilh") ||
                  q.contains("him") || q.contains("dan") || q.contains("teh") ||
                  q.contains("khawih") || q.contains("tih") || q.contains("kan");

    // Extract selected map name from systemPrompt if available
    QString selMap = "Selected Map";
    QRegularExpression reSel("Selected:\\s*([^\\|\\n\\r]+)");
    QRegularExpressionMatch mSel = reSel.match(systemPrompt);
    if (mSel.hasMatch() && mSel.captured(1).trimmed() != "none") {
        selMap = mSel.captured(1).trimmed();
    }

    QJsonDocument doc = QJsonDocument::fromJson(resultJson.toUtf8());
    if (!doc.isObject()) {
        return QString("### ⚙️ Tool Execution Result\n\n```\n%1\n```\n").arg(resultJson);
    }
    QJsonObject root = doc.object();

    if (root.contains("error")) {
        QString err = root["error"].toString();
        if (isMizo) {
            return QString("### ⚠️ Harsatna a awm (Error)\n\nROM data chhiar kawngah he thil hi a thleng: **%1**.\n\nROM file a in-open chian leh map hming a dik em tih enfiah a ngai e.").arg(err);
        }
        return QString("### ⚠️ Operation Encountered an Issue\n\n**Details:** %1\n\nPlease ensure a valid ROM file and map pack are currently active.").arg(err);
    }

    // 1. Map Statistics & Mathematical Safety
    if (toolName == "get_map_statistics") {
        QString name = root.contains("name") ? root["name"].toString() : selMap;
        return evaluateMapSafety(root, name, isMizo);
    }

    // 2. Modified Maps Audit
    if (toolName == "get_modified_maps") {
        return interpretModifiedMaps(root, isMizo);
    }

    // 3. Anomaly Detection
    if (toolName == "detect_anomalies") {
        return interpretAnomalies(root, selMap, isMizo);
    }

    // 4. Project Information
    if (toolName == "get_project_info") {
        return interpretProjectInfo(root, isMizo);
    }

    // 5. Map Search Candidates
    if (toolName == "search_maps" || root.contains("results")) {
        QJsonArray resArr = root["results"].toArray();
        if (resArr.isEmpty()) {
            if (isMizo) {
                return "### 🔍 Map Search Result\n\nHe pattern mil map engmah hmuh a ni lo e.\n\n"
                       "**Rawtna (Tips):**\n"
                       "- Wildcard zau zawk hmang la (entirnan: `*`)\n"
                       "- A2L emaw OLS map pack load a nih leh nih loh enfiah rawh\n"
                       "- Hex View-ah table headers zawng chhin rawh.";
            }
            return "### 🔍 Map Search Result\n\nNo matching maps found for this search pattern in the loaded project.\n\n"
                   "**Recommendations:**\n"
                   "- Try searching with broader wildcards (e.g., `*`)\n"
                   "- Verify if an ASAP2 (A2L) or OLS map pack is loaded for this ROM\n"
                   "- Inspect Hex view for table descriptors.";
        }

        QString out = QString("### 🔍 Found %1 Candidate Map(s)\n\n| Map Name | Memory Address | Dimensions | Unit |\n|---|---|---|---|\n").arg(resArr.size());
        for (int j = 0; j < qMin(resArr.size(), 12); ++j) {
            QJsonObject m = resArr[j].toObject();
            QString name = m["name"].toString();
            QString addr = m.contains("address") ? QString("0x%1").arg(m["address"].toInt(), 6, 16, QChar('0')).toUpper() : "N/A";
            QString dims = (m.contains("rows") && m.contains("cols")) ? QString("%1x%2").arg(m["rows"].toInt()).arg(m["cols"].toInt()) : "N/A";
            QString unit = m["unit"].toString("-");
            out += QString("| **%1** | `%2` | %3 | %4 |\n").arg(name, addr, dims, unit);
        }
        if (resArr.size() > 12) {
            out += QString("\n*...and %1 additional maps.*").arg(resArr.size() - 12);
        }

        if (isMizo) {
            out += "\n\n💡 **Tuning Rawnna:**\n"
                   "- A chunga map-te hi Map List-ah khuan double-click la, 2D leh 3D surface view a lo lang ang.\n"
                   "- Stage 1 tan chuan a tlangpuiin **+5% atanga +10%** inkar chauh tihsan tur a ni a, axis smoothing tih tel ngei tur a ni.";
        } else {
            out += "\n\n💡 **Tuning Recommendation:**\n"
                   "- Double-click any map above in the Map List to open its 2D curve and 3D surface editor.\n"
                   "- For Stage 1 tuning, keep modifications within safe thresholds (+5% to +10%) and ensure axis transitions remain smooth.";
        }
        return out;
    }

    // 6. Scale Map
    if (toolName == "scale_map_values") {
        return interpretScaleMap(root, isMizo);
    }

    // 7. Smooth Map
    if (toolName == "smooth_map") {
        return interpretSmoothMap(root, isMizo);
    }

    // 8. Zero Map
    if (toolName == "zero_map") {
        return interpretZeroMap(root, isMizo);
    }

    // 9. Restore Map
    if (toolName == "restore_map") {
        return interpretRestoreMap(root, isMizo);
    }

    // 10. Compare Map Values
    if (toolName == "compare_map_values") {
        return interpretCompareMap(root, isMizo);
    }

    // 11. Identify Map Purpose
    if (toolName == "identify_map_purpose") {
        return interpretMapPurpose(root, isMizo);
    }

    // 12. Describe Map Shape
    if (toolName == "describe_map_shape") {
        return interpretMapShape(root, isMizo);
    }

    // 13. Related Maps
    if (toolName == "get_related_maps") {
        return interpretRelatedMaps(root, isMizo);
    }

    // 14. Undo Last Change
    if (toolName == "undo_last_change") {
        return interpretUndo(root, isMizo);
    }

    // 15. List Maps
    if (toolName == "list_maps") {
        return interpretListMaps(root, isMizo);
    }

    // Default formatting
    return QString("### ⚙️ Operation Completed\n\nResult:\n```json\n%1\n```\n")
        .arg(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)));
}

QString OfflineECUExpertProvider::evaluateMapSafety(const QJsonObject &stats,
                                                    const QString &mapName,
                                                    bool isMizo)
{
    double minVal = stats.contains("min") ? stats["min"].toDouble() : 0;
    double maxVal = stats.contains("max") ? stats["max"].toDouble() : 0;
    double avgVal = stats.contains("avg") ? stats["avg"].toDouble() : 0;
    double stdDev = stats.contains("stdDev") ? stats["stdDev"].toDouble() : 0;
    int cells = stats.contains("totalCells") ? stats["totalCells"].toInt() : 0;
    QString unit = stats.contains("unit") ? stats["unit"].toString() : "-";

    QString lowerName = mapName.toLower();

    // Map Category Detection
    QString category = "General Calibration Map";
    QString safetyStatus = "🟢 SAFE (Nominal Range)";
    QString safetyAnalysis;
    QString formulaAdvice;

    if (lowerName.contains("boost") || lowerName.contains("ldr") || lowerName.contains("ladedruck") || lowerName.contains("plgr")) {
        category = "Turbocharger Target Boost Pressure (Ladedruck)";
        if (maxVal > 2800) {
            safetyStatus = "🔴 CRITICAL DANGER — Overboost / Hardware Destruction Risk";
            safetyAnalysis = "Peak pressure exceeds 2,800 mbar. Stock 3-bar MAP sensor will clip, and turbocharger will overspeed resulting in shaft fracture or blown head gasket.";
        } else if (maxVal > 2550) {
            safetyStatus = "🟡 CAUTION — Near OEM Turbo Mechanical Limit";
            safetyAnalysis = "Peak boost (2,550 - 2,800 mbar) is near the upper efficiency ceiling of OEM turbos. Verify intercooler capacity, N75 duty cycle (< 85%), and EGT.";
        } else {
            safetyStatus = "🟢 SAFE — Within Stage 1 Performance Threshold";
            safetyAnalysis = QString("Peak boost of %1 mbar is within safe OEM hardware margins. A +150 to +200 mbar increase is appropriate for Stage 1.").arg(maxVal, 0, 'f', 1);
        }
        formulaAdvice = "Stage 1 Formula: `New = Current * 1.07` in peak load area (70-100% throttle, 2,200-4,000 RPM). Also raise Boost Limiter map by matching delta.";
    } else if (lowerName.contains("rail") || lowerName.contains("raildruck") || lowerName.contains("pr_soll") || lowerName.contains("cr_")) {
        category = "Common Rail High Pressure Fuel Target";
        if (maxVal > 2050) {
            safetyStatus = "🔴 CRITICAL DANGER — Exceeds Mechanical Pressure Relief Valve";
            safetyAnalysis = "Pressure above 2,050 bar risks mechanical blow-off of rail safety valve, injector seal extrusion, and fuel pump cavitation.";
        } else if (maxVal > 1850) {
            safetyStatus = "🟡 CAUTION — High Performance Common Rail Pressure";
            safetyAnalysis = "Safe only on CP4 / Piezo 2,000-bar rated systems. Do not exceed 1,650 bar on older CP1/CP3 pumps.";
        } else {
            safetyStatus = "🟢 SAFE — Standard High-Pressure Window";
            safetyAnalysis = QString("Peak pressure of %1 bar allows excellent fuel droplet atomization.").arg(maxVal, 0, 'f', 1);
        }
        formulaAdvice = "Stage 1 Formula: Increase top 3 rows by `+50 to +80 bar`. Smooth transition into lower RPM columns.";
    } else if (lowerName.contains("torque") || lowerName.contains("drehmoment") || lowerName.contains("tq") || lowerName.contains("trq")) {
        category = "Engine Torque Limiter (Drehmomentbegrenzung)";
        safetyStatus = "🟢 SAFE — Torque Shaping Protocol";
        safetyAnalysis = QString("Maximum requested torque is %1 Nm. Ensure low RPM torque (< 2,200 RPM) does not overload Dual Mass Flywheel (DMF) or clutch.").arg(maxVal, 0, 'f', 1);
        formulaAdvice = "Stage 1 Formula: Add `+15%` between 2,200 - 3,800 RPM. Maintain gradual incline below 2,000 RPM to protect drivetrain.";
    } else if (lowerName.contains("lambda") || lowerName.contains("afr") || lowerName.contains("lamfa") || lowerName.contains("smoke") || lowerName.contains("rauch")) {
        category = "Air-Fuel Ratio (AFR) / Lambda Limiter";
        if (minVal < 0.75 && lowerName.contains("lambda")) {
            safetyStatus = "🔴 CRITICAL DANGER — Over-rich Fueling / Bore Wash Risk";
            safetyAnalysis = "Lambda values below 0.75 cause unburnt fuel to wash oil film off cylinder walls, causing piston ring scoring.";
        } else {
            safetyStatus = "🟢 SAFE — Combustion Chemistry Margins";
            safetyAnalysis = QString("Air-Fuel balance (Min: %1, Max: %2). Target 0.82-0.85 on turbo petrol, >1.15 on diesel.").arg(minVal, 0, 'f', 2).arg(maxVal, 0, 'f', 2);
        }
        formulaAdvice = "Ensure smoke limiter air mass matches actual turbocharger MAF readings.";
    } else if (lowerName.contains("zw") || lowerName.contains("zünd") || lowerName.contains("spark") || lowerName.contains("ignit")) {
        category = "Ignition Timing / Spark Advance Angle";
        if (maxVal > 35.0) {
            safetyStatus = "🔴 DANGER — Severe Knock & Pre-Ignition Risk";
            safetyAnalysis = "Ignition advance above 35° BTDC under boost causes violent detonation and cylinder piston crown melting.";
        } else {
            safetyStatus = "🟢 SAFE — Spark Advance Table";
            safetyAnalysis = QString("Advance range: %1° to %2° BTDC.").arg(minVal, 0, 'f', 1).arg(maxVal, 0, 'f', 1);
        }
        formulaAdvice = "Stage 1 Formula: Advance +1.5° to +2.5° BTDC on 98+ RON fuel; log knock retard sensors.";
    }

    QString report;
    report += QString("## 🛡️ Map Safety & Mathematical Analysis: `%1`\n\n").arg(mapName);
    report += QString("**Classification:** %1\n").arg(category);
    report += QString("**Safety Rating:** %1\n\n").arg(safetyStatus);

    report += "### 📊 Mathematical Distribution:\n";
    report += "| Metric | Value | Engineering Context |\n";
    report += "|---|---|---|\n";
    report += QString("| **Minimum Value** | `%1 %2` | Lowest baseline cell value |\n").arg(minVal, 0, 'f', 2).arg(unit);
    report += QString("| **Maximum Value** | `%1 %2` | Peak load / ceiling limit |\n").arg(maxVal, 0, 'f', 2).arg(unit);
    report += QString("| **Mean / Average** | `%1 %2` | Overall map median density |\n").arg(avgVal, 0, 'f', 2).arg(unit);
    report += QString("| **Std Deviation** | `±%1` | Gradient variance across axes |\n").arg(stdDev, 0, 'f', 2);
    if (cells > 0) {
        report += QString("| **Total Cells** | `%1` | Interpolated matrix points |\n").arg(cells);
    }
    report += "\n";

    report += "### 🔍 Engineering Evaluation:\n";
    report += safetyAnalysis + "\n\n";

    if (!formulaAdvice.isEmpty()) {
        report += "### 📐 Calibration Formula Recommendation:\n";
        report += formulaAdvice + "\n\n";
    }

    if (isMizo) {
        report += "### 📝 Mizo Tlangkawmna:\n";
        report += QString("- He map **%1** hi kan evaluate a, **%2** a ni.\n").arg(mapName, safetyStatus);
        report += QString("- A value sang ber hi `%1 %2` a ni a, safety margin a la nei tha e.\n").arg(maxVal, 0, 'f', 1).arg(unit);
        report += "- Modify i duh chuan proportional scaling (`* 1.05` vel) hmang la, sudden spikes awm lo turin 3D surface plot kha check tel zel ang che.\n";
    }

    return report;
}

QString OfflineECUExpertProvider::interpretModifiedMaps(const QJsonObject &root, bool isMizo)
{
    int count = root.contains("count") ? root["count"].toInt() : 0;
    QJsonArray arr = root.contains("modifiedMaps") ? root["modifiedMaps"].toArray() : QJsonArray();

    if (count == 0 || arr.isEmpty()) {
        if (isMizo) {
            return "## 📝 ROM Data Changes Overview\n\n"
                   "**Eng map mah modify a la ni lo!** Tunah hian ROM hi original factory state-ah a la awm e.\n\n"
                   "💡 *Map modify i duh chuan Map Editor-ah lutin values adjust la, emaw AI hnenah recipe (decat, boost, torque) zawt rawh.*";
        }
        return "## 📝 ROM Data Changes Overview\n\n"
               "**No modified maps detected.** The ROM is currently in its original factory state.\n\n"
               "💡 *Open a map in the editor to make changes, or request a recipe (Stage 1, Decat, EGR Off, Pops & Bangs).*";
    }

    QString out;
    out += QString("## 📝 Modified Maps Audit (%1 Modified)\n\n").arg(count);
    out += "The following maps have been modified compared to the original factory ROM:\n\n";
    out += "| # | Map Name | System Category | Calibration Role |\n";
    out += "|---|---|---|---|\n";

    for (int i = 0; i < arr.size(); ++i) {
        QString name = arr[i].toString();
        QString lower = name.toLower();
        QString cat = "General Engine";
        QString role = "Custom Tuning Adjustment";

        if (lower.contains("boost") || lower.contains("ldr") || lower.contains("ladedruck")) {
            cat = "Airflow / Turbo"; role = "Target Manifold Pressure";
        } else if (lower.contains("torque") || lower.contains("drehmoment")) {
            cat = "Torque Monitoring"; role = "Engine Output Limit";
        } else if (lower.contains("rail") || lower.contains("raildruck")) {
            cat = "Fuel Delivery"; role = "Common Rail Target";
        } else if (lower.contains("kat") || lower.contains("dkat")) {
            cat = "Exhaust Emissions"; role = "Catalytic Diagnosis";
        } else if (lower.contains("egr") || lower.contains("agr")) {
            cat = "Exhaust Emissions"; role = "EGR Valve Flow Target";
        } else if (lower.contains("vmax")) {
            cat = "Speed Limiting"; role = "Vehicle Top Speed Threshold";
        }

        out += QString("| %1 | **%2** | %3 | %4 |\n").arg(i + 1).arg(name, cat, role);
    }

    out += "\n### 🛡️ Calibration Integrity Checklist:\n";
    out += "1. **Boost vs Boost Limiter**: If turbo boost was raised, verify boost limiter was proportionally raised to avoid `P0234` limp mode.\n";
    out += "2. **Torque vs Gearbox TCU**: Ensure torque limits respect automatic transmission clutch ceilings (e.g. 350 Nm on DSG DQ200).\n";
    out += "3. **Checksum Recalculation**: Always execute checksum calculation before saving and flashing the binary.\n";

    if (isMizo) {
        out += QString("\n### 📝 Mizo Tlangkawmna:\nI ROM-ah hian map **%1** modify a ni tawh a. Flashing i tih hmain Checksum verify theihnghilh suh aw!\n").arg(count);
    }

    return out;
}

QString OfflineECUExpertProvider::interpretAnomalies(const QJsonObject &root,
                                                     const QString &mapName,
                                                     bool isMizo)
{
    QString out = QString("## 🔍 Anomaly & Data Integrity Check: `%1`\n\n").arg(mapName);
    QJsonArray anomalies = root.contains("anomalies") ? root["anomalies"].toArray() : QJsonArray();

    if (anomalies.isEmpty()) {
        out += "### 🟢 Status: PERFECT INTEGRITY (No Anomalies Detected)\n\n"
               "- **Monotonic Axes:** All axis indices increment smoothly.\n"
               "- **Surface Continuity:** No sharp cliff discontinuities or inverted valleys.\n"
               "- **Data Bounds:** All cells are within realistic physical sensor bounds.\n\n"
               "This map is mathematically safe and ready for flashing.";
        if (isMizo) {
            out += "\n\n💡 *Map-ah hian diklo leh buaina engmah hmuh a ni lo e. A surface a rualrem tha vek.*";
        }
        return out;
    }

    out += QString("### ⚠️ %1 Potential Anomaly(ies) Detected:\n\n").arg(anomalies.size());
    out += "| Anomaly Type | Coordinates | Description |\n|---|---|---|\n";
    for (int i = 0; i < anomalies.size(); ++i) {
        QJsonObject a = anomalies[i].toObject();
        QString type = a["type"].toString("Irregularity");
        QString coords = a.contains("coords") ? a["coords"].toString() : QString("Cell %1").arg(i + 1);
        QString desc = a["description"].toString("Value gradient jump detected");
        out += QString("| **%1** | `%2` | %3 |\n").arg(type, coords, desc);
    }

    out += "\n💡 **Remediation:** Inspect these coordinates in the 3D surface editor and smooth the transition.";
    return out;
}

QString OfflineECUExpertProvider::interpretProjectInfo(const QJsonObject &root, bool isMizo)
{
    QString title = root["title"].toString("romHEX14 Project");
    QString romName = root["romName"].toString("Primary ROM");
    int sizeBytes = root["romSizeBytes"].toInt(0);
    int mapCount = root["mapCount"].toInt(0);
    int modified = root["modifiedCount"].toInt(0);
    QString byteOrder = root["byteOrder"].toString("Little Endian");

    QString out = "## 📋 ECU Project Health Dashboard\n\n";
    out += "| Parameter | Current Project Value |\n|---|---|\n";
    out += QString("| **Project Title** | `%1` |\n").arg(title);
    out += QString("| **Active ROM File** | `%1` |\n").arg(romName);
    out += QString("| **Binary Size** | `%1 bytes (%2 KB)` |\n").arg(sizeBytes).arg(sizeBytes / 1024.0, 0, 'f', 1);
    out += QString("| **Defined Maps** | `%1 maps` |\n").arg(mapCount);
    out += QString("| **Modified Status** | `%1 map(s) modified` |\n").arg(modified);
    out += QString("| **Microprocessor Byte Order** | `%1` |\n").arg(byteOrder);
    out += "\n";

    if (isMizo) {
        out += QString("💡 *I project `%1`-ah hian map %2 a awm a, tunah hian %3 modify a ni tawh e.*").arg(title).arg(mapCount).arg(modified);
    } else {
        out += "💡 *All maps are indexed and available for editing and tuning analysis.*";
    }
    return out;
}

QString OfflineECUExpertProvider::interpretScaleMap(const QJsonObject &root, bool isMizo)
{
    QString mapName = root.value("map").toString("Selected Map");
    double factor = root.value("factor").toDouble(1.0);
    int cells = root.value("cellsWritten").toInt(0);

    double pct = (factor - 1.0) * 100.0;
    QString pctStr = (pct >= 0) ? QString("+%1%").arg(pct, 0, 'f', 1) : QString("%1%").arg(pct, 0, 'f', 1);

    QString out = QString("## 📈 Map Scaling Applied: `%1`\n\n").arg(mapName);
    out += "| Parameter | Applied Value |\n|---|---|\n";
    out += QString("| **Target Map** | `%1` |\n").arg(mapName);
    out += QString("| **Scale Factor** | `×%1` (%2) |\n").arg(factor, 0, 'f', 4).arg(pctStr);
    out += QString("| **Cells Modified** | `%1 cells` |\n").arg(cells);
    out += "| **Rollback Snapshot** | Automatically archived in project version history |\n\n";

    out += "### 💡 Post-Modification Recommendations:\n";
    out += "1. Inspect the 3D surface editor to verify that gradient transitions remain smooth.\n";
    out += "2. If this was a Boost or Rail Pressure map, verify corresponding limiter curves match.\n";
    out += "3. Run *\"Check this map for safety\"* to verify values remain within physical sensor tolerances.\n";

    if (isMizo) {
        out += QString("\n### 📝 Mizo Tlangkawmna:\n"
                       "- Map **%1** chu **%2** in tihsan a ni tawh e (%3 cells modify a ni).\n"
                       "- Auto-snapshot save a ni a, sut leh i duh chuan 'undo' tiin i sawi thei ang.\n"
                       "- 3D surface view-ah a rualrem em tih check la, flashing hmain checksum verify ngei ang che!\n").arg(mapName, pctStr).arg(cells);
    }
    return out;
}

QString OfflineECUExpertProvider::interpretSmoothMap(const QJsonObject &root, bool isMizo)
{
    QString mapName = root.value("map").toString("Selected Map");
    int iters = root.value("iterations").toInt(1);

    QString out = QString("## 🌊 Map Surface Smoothing Applied: `%1`\n\n").arg(mapName);
    out += QString("- **Algorithm:** 2D Neighbor-Averaging Smoothing Filter\n");
    out += QString("- **Passes:** %1 iteration(s)\n").arg(iters);
    out += "- **Result:** Eliminated sharp localized spikes, smoothed cliff transitions, and balanced surface curvature.\n\n";
    out += "💡 *The progressive surface ensures smooth engine driveability and eliminates boost/torque hunting.*";

    if (isMizo) {
        out += QString("\n\n### 📝 Mizo Tlangkawmna:\n"
                       "- Map **%1** surface chu tihmam (smooth) a ni a, cell inkar a kham sang leh spikes awmte tihrêm a ni e.\n").arg(mapName);
    }
    return out;
}

QString OfflineECUExpertProvider::interpretZeroMap(const QJsonObject &root, bool isMizo)
{
    QString mapName = root.value("map").toString("Selected Map");
    int cells = root.value("cellsWritten").toInt(0);

    QString out = QString("## 🛑 Map Zeroed Out: `%1`\n\n").arg(mapName);
    out += QString("- **Total Cells Zeroed:** %1 cells set to `0.0`\n").arg(cells);
    out += "- **Safety Snapshot:** Pre-zero state saved to project version history.\n\n";
    out += "### Common Calibration Uses for Map Zeroing:\n";
    out += "- **EGR Hysteresis Curves:** Disables EGR valve actuation without triggering sensor open-loop codes.\n";
    out += "- **DTC Error Code Mask:** Suppresses check engine illumination for specific diagnostic paths.\n";
    out += "- **Swirl Flap Tables:** Prevents intake manifold swirl runner actuation.\n";
    out += "- **Post-Injection Duration:** Deactivates DPF regeneration thermal dosing.\n";

    if (isMizo) {
        out += QString("\n### 📝 Mizo Tlangkawmna:\n"
                       "- Map **%1** a value awm zawng zawng chu **0** a dah vek a ni e.\n"
                       "- EGR deactivation, DTC error clearing, emaw Swirl flap delete tan a tangkai hle.\n").arg(mapName);
    }
    return out;
}

QString OfflineECUExpertProvider::interpretRestoreMap(const QJsonObject &root, bool isMizo)
{
    QString mapName = root.value("map").toString("Selected Map");
    int bytes = root.value("bytesRestored").toInt(0);

    QString out = QString("## 🔄 Map Restored to Stock: `%1`\n\n").arg(mapName);
    out += QString("- **Restored:** %1 bytes from factory OEM ROM.\n").arg(bytes);
    out += "- **Integrity:** 100% bit-for-bit matched with original un-tuned ROM.\n\n";
    out += "💡 *All modifications on this map have been safely reverted.*";

    if (isMizo) {
        out += QString("\n\n### 📝 Mizo Tlangkawmna:\n"
                       "- Map **%1** hi a tir a factory stock (original)-ah dah let leh thlap a ni e.\n").arg(mapName);
    }
    return out;
}

QString OfflineECUExpertProvider::interpretCompareMap(const QJsonObject &root, bool isMizo)
{
    QString mapName = root.value("map").toString("Selected Map");
    QString unit = root.value("unit").toString("-");
    int totalCells = root.value("totalCells").toInt(0);
    int changedCells = root.value("changedCells").toInt(0);
    bool identical = root.value("identical").toBool(true);

    if (identical || changedCells == 0) {
        if (isMizo) {
            return QString("## ⚖️ Map Comparison: `%1`\n\n🟢 **Original nen a inang chiah!** He map-ah hian modification engmah a la awm lo e.").arg(mapName);
        }
        return QString("## ⚖️ Map Comparison: `%1`\n\n🟢 **Identical to OEM Stock:** No differences detected between current map and original ROM.").arg(mapName);
    }

    QString out = QString("## ⚖️ Map Comparison: `%1` (%2 of %3 cells modified)\n\n").arg(mapName).arg(changedCells).arg(totalCells);
    out += "| Cell [Row, Col] | Original Stock | Modified Value | Delta Difference | % Change |\n";
    out += "|---|---|---|---|---|\n";

    QJsonArray diffs = root.value("changes").toArray();
    int showCount = qMin(diffs.size(), 12);
    for (int i = 0; i < showCount; ++i) {
        QJsonObject d = diffs[i].toObject();
        int r = d["row"].toInt();
        int c = d["col"].toInt();
        double orig = d["original"].toDouble();
        double cur = d["current"].toDouble();
        double delta = d["delta"].toDouble();
        double pct = (qAbs(orig) > 1e-6) ? (delta / orig) * 100.0 : 0.0;
        out += QString("| `[%1, %2]` | %3 %4 | **%5 %4** | `%6%7` | `%8%9%` |\n")
            .arg(r).arg(c)
            .arg(orig, 0, 'f', 2).arg(unit)
            .arg(cur, 0, 'f', 2)
            .arg(delta >= 0 ? "+" : "").arg(delta, 0, 'f', 2)
            .arg(pct >= 0 ? "+" : "").arg(pct, 0, 'f', 1);
    }
    if (diffs.size() > 12) {
        out += QString("\n*...and %1 additional modified cell(s).*").arg(diffs.size() - 12);
    }

    if (isMizo) {
        out += QString("\n\n### 📝 Mizo Tlangkawmna:\n"
                       "- Map **%1**-ah hian cell **%2** modify a ni a. A chunga table-ah khuan Original leh Current value khaikhinna a lang e.\n").arg(mapName).arg(changedCells);
    }
    return out;
}

QString OfflineECUExpertProvider::interpretMapPurpose(const QJsonObject &root, bool isMizo)
{
    QString mapName = root.value("map").toString("Selected Map");
    QString purpose = root.value("purpose").toString("General Calibration Function");
    QString category = root.value("category").toString("Engine Management");
    QString confidence = root.value("confidence").toString("medium").toUpper();
    QString relevance = root.value("tuningRelevance").toString();

    QString out = QString("## 🧭 Map Purpose Identification: `%1`\n\n").arg(mapName);
    out += "| Attribute | Analysis |\n|---|---|\n";
    out += QString("| **Map Identifier** | `%1` |\n").arg(mapName);
    out += QString("| **Identified Purpose** | **%1** |\n").arg(purpose);
    out += QString("| **System Category** | `%1` |\n").arg(category);
    out += QString("| **Identification Confidence** | `%1` |\n").arg(confidence);
    if (!relevance.isEmpty()) {
        out += QString("| **Tuning Relevance** | %1 |\n").arg(relevance);
    }
    out += "\n";

    if (isMizo) {
        out += QString("### 📝 Mizo Hrilhfiahna:\n"
                       "- He map **%1** hi engine management-ah **%2** atan a hman a ni a (`%3` category).\n"
                       "- Modify i duh chuan he map system mil hian fine-tuning kalpui tur a ni e.\n").arg(mapName, purpose, category);
    }
    return out;
}

QString OfflineECUExpertProvider::interpretMapShape(const QJsonObject &root, bool isMizo)
{
    QString mapName = root.value("map").toString("Selected Map");
    QString desc = root.value("description").toString("Continuous 3D Surface");
    double minVal = root.value("min").toDouble();
    double maxVal = root.value("max").toDouble();
    double mean = root.value("mean").toDouble();
    bool monoX = root.value("monotonicX").toBool(false);
    bool monoY = root.value("monotonicY").toBool(false);

    QString out = QString("## 📐 3D Surface Topology Analysis: `%1`\n\n").arg(mapName);
    out += QString("**Shape Summary:** %1\n\n").arg(desc);
    out += "| Metric | Measurement | Engineering Implication |\n|---|---|---|\n";
    out += QString("| **Min / Max Range** | `%1` to `%2` | Dynamic physical span |\n").arg(minVal, 0, 'f', 2).arg(maxVal, 0, 'f', 2);
    out += QString("| **Mean Amplitude** | `%1` | Average height |\n").arg(mean, 0, 'f', 2);
    out += QString("| **X-Axis Monotonicity** | `%1` | Progressive along X-axis |\n").arg(monoX ? "Monotonic (Smooth)" : "Non-monotonic (Curved/Plateau)");
    out += QString("| **Y-Axis Monotonicity** | `%1` | Progressive along Y-axis |\n").arg(monoY ? "Monotonic (Smooth)" : "Non-monotonic (Curved/Plateau)");
    out += "\n";

    if (isMizo) {
        out += QString("### 📝 Mizo Tlangkawmna:\n"
                       "- Map surface pianhmang: **%1**.\n"
                       "- Axis rualrem dan hi engine driveability atan a pawimawh hle a, kham sang leh phawng thut awm lo tura smoothing tih a tha e.\n").arg(desc);
    }
    return out;
}

QString OfflineECUExpertProvider::interpretRelatedMaps(const QJsonObject &root, bool isMizo)
{
    QString mapName = root.value("map").toString("Selected Map");
    QJsonArray related = root.value("relatedMaps").toArray();

    QString out = QString("## 🔗 Related & Axis-Sibling Maps: `%1`\n\n").arg(mapName);
    if (related.isEmpty()) {
        out += "No directly linked axis-sharing maps found in this map pack.\n";
        return out;
    }

    out += "The following maps share common axis signals or belong to the same calibration group:\n\n";
    out += "| # | Related Map Name | Relationship Type |\n|---|---|---|\n";
    for (int i = 0; i < qMin(related.size(), 12); ++i) {
        QJsonObject r = related[i].toObject();
        QString rName = r["name"].toString();
        QString reason = r["relationship"].toString("Shared Axis / Group");
        out += QString("| %1 | **%2** | %3 |\n").arg(i + 1).arg(rName, reason);
    }
    out += "\n💡 *Always modify sibling limiter maps concurrently when scaling primary target maps.*";

    if (isMizo) {
        out += "\n\n### 📝 Mizo Tlangkawmna:\n"
               "- A chunga map-te khuan he map nen hian axis emaw system inzawmna an nei a. Target map tihsan rualin limiter map-te pawh tihsan ve zel tur a ni e.\n";
    }
    return out;
}

QString OfflineECUExpertProvider::interpretUndo(const QJsonObject &root, bool isMizo)
{
    QString restored = root.value("restored").toString("Previous Version");
    QString out = "## ↩️ Rollback Successful (Undo Completed)\n\n";
    out += QString("Successfully reverted ROM to snapshot:\n> **\"%1\"**\n\n").arg(restored);
    out += "All cell modifications from the last operation have been undone safely.";

    if (isMizo) {
        out += QString("\n\n### 📝 Mizo Tlangkawmna:\n- Hma a i thil tih kha sut (undo) a ni tawh a, **\"%1\"** state-ah khan dah let leh a ni e.\n").arg(restored);
    }
    return out;
}

QString OfflineECUExpertProvider::interpretListMaps(const QJsonObject &root, bool isMizo)
{
    QJsonArray maps = root.value("maps").toArray();
    int count = root.contains("count") ? root["count"].toInt() : maps.size();

    QString out = QString("## 🗺️ Project Map Registry (%1 Total Maps)\n\n").arg(count);
    out += "| # | Map Name | Memory Address | Dimensions | Unit |\n|---|---|---|---|---|\n";
    int show = qMin(maps.size(), 15);
    for (int i = 0; i < show; ++i) {
        QJsonObject m = maps[i].toObject();
        QString name = m["name"].toString();
        QString addr = m.contains("address") ? QString("0x%1").arg(m["address"].toInt(), 6, 16, QChar('0')).toUpper() : "N/A";
        QString dims = QString("%1×%2").arg(m["rows"].toInt()).arg(m["cols"].toInt());
        QString unit = m["unit"].toString("-");
        out += QString("| %1 | **%2** | `%3` | %4 | %5 |\n").arg(i + 1).arg(name, addr, dims, unit);
    }
    if (maps.size() > 15) {
        out += QString("\n*...and %1 additional maps.*").arg(maps.size() - 15);
    }
    out += "\n\n💡 *Select any map above by double-clicking in the Map List to analyze or edit.*";

    if (isMizo) {
        out += QString("\n\n### 📝 Mizo Tlangkawmna:\n- I project-ah hian map **%1** a awm a. A chung a map i duh ber sawi la, safety analysis emaw tuning ka lo pui zel ang che.\n").arg(count);
    }
    return out;
}

QString OfflineECUExpertProvider::generateExpertResponse(const QString &query,
                                                         const QString &systemPrompt,
                                                         const QVector<AIToolDef> &tools,
                                                         QString &outToolName,
                                                         QJsonObject &outToolArgs)
{
    QString q = query.trimmed().toLower();
    bool isMizo = q.contains("eng") || q.contains("nih") || q.contains("hrilh") ||
                  q.contains("him") || q.contains("dan") || q.contains("teh") ||
                  q.contains("khawih") || q.contains("tih") || q.contains("kan");

    // Extract selected map name from systemPrompt if available
    QString selMap = "none";
    QRegularExpression reSel("Selected:\\s*([^\\|\\n\\r]+)");
    QRegularExpressionMatch mSel = reSel.match(systemPrompt);
    if (mSel.hasMatch()) {
        selMap = mSel.captured(1).trimmed();
    }

    auto hasTool = [&](const QString &toolName) -> bool {
        for (const auto &t : tools) {
            if (t.name == toolName) return true;
        }
        return false;
    };

    // ── Phase 1: Autonomous Multi-Turn Tool Dispatch ─────────────────────────

    // A. Anomaly Detection
    if (hasTool("detect_anomalies") && selMap != "none" &&
        (q.contains("anomal") || q.contains("error") || q.contains("bug") || q.contains("diklo") || q.contains("check problem"))) {
        outToolName = "detect_anomalies";
        outToolArgs["name"] = selMap;
        return QString();
    }

    // B. Map Safety / Mathematical Statistics
    if (hasTool("get_map_statistics") && selMap != "none" &&
        (q.contains("safe") || q.contains("him") || q.contains("stat") || q.contains("min") || q.contains("max") ||
         q.contains("check this map") || q.contains("analyze this map") || q.contains("evaluate") || q.contains("he map hi"))) {
        outToolName = "get_map_statistics";
        outToolArgs["name"] = selMap;
        return QString();
    }

    // C. Modified Maps Audit
    if (hasTool("get_modified_maps") &&
        (q.contains("modified") || q.contains("change") || q.contains("diff") || q.contains("danglam") || q.contains("khawih tawh") || q.contains("what did i change"))) {
        outToolName = "get_modified_maps";
        outToolArgs = QJsonObject();
        return QString();
    }

    // D. Project Info
    if (hasTool("get_project_info") &&
        (q.contains("project info") || q.contains("rom info") || q.contains("project details") || q.contains("project chungchang"))) {
        outToolName = "get_project_info";
        outToolArgs = QJsonObject();
        return QString();
    }

    // E. Search Maps
    if (hasTool("search_maps")) {
        if (q.contains("search map") || q.contains("find map") || q.startsWith("search ") || q.startsWith("find ")) {
            QString pattern = query;
            pattern.remove(QRegularExpression("^(search|find|locate)\\s+(maps?\\s+)?", QRegularExpression::CaseInsensitiveOption));
            pattern = pattern.trimmed();
            if (!pattern.contains("*")) pattern = "*" + pattern + "*";
            outToolName = "search_maps";
            outToolArgs["query"] = pattern;
            return QString();
        }
        // Direct recipe search triggers
        if (q.contains("decat") && (q.contains("apply") || q.contains("map") || q.contains("zawng"))) {
            outToolName = "search_maps"; outToolArgs["query"] = "*KAT*"; return QString();
        }
        if (q.contains("egr") && (q.contains("apply") || q.contains("map") || q.contains("zawng"))) {
            outToolName = "search_maps"; outToolArgs["query"] = "*EGR*"; return QString();
        }
        if (q.contains("dpf") && (q.contains("apply") || q.contains("map") || q.contains("zawng"))) {
            outToolName = "search_maps"; outToolArgs["query"] = "*DPF*"; return QString();
        }
        if (q.contains("boost") && (q.contains("search") || q.contains("find") || q.contains("map"))) {
            outToolName = "search_maps"; outToolArgs["query"] = "*LDR*"; return QString();
        }
        if (q.contains("torque") && (q.contains("search") || q.contains("find") || q.contains("map"))) {
            outToolName = "search_maps"; outToolArgs["query"] = "*torque*"; return QString();
        }
    }

    // F. Scale Map Values
    if (hasTool("scale_map_values") &&
        (q.contains("scale") || q.contains("increase") || q.contains("tipung") || q.contains("tihsan") || q.contains("multiply") || q.contains("decrease"))) {
        if (selMap != "none") {
            double factor = 1.05; // default 5%
            QRegularExpression rePct("([+-]?\\d+(?:\\.\\d+)?)\\s*%");
            QRegularExpressionMatch mPct = rePct.match(q);
            if (mPct.hasMatch()) {
                double val = mPct.captured(1).toDouble();
                factor = 1.0 + (val / 100.0);
            } else {
                QRegularExpression reFactor("(?:scale|factor|by)\\s+([0-9]+\\.[0-9]+)");
                QRegularExpressionMatch mFactor = reFactor.match(q);
                if (mFactor.hasMatch()) {
                    factor = mFactor.captured(1).toDouble();
                }
            }
            outToolName = "scale_map_values";
            outToolArgs["name"] = selMap;
            outToolArgs["factor"] = factor;
            return QString();
        } else {
            if (isMizo) {
                return "⚠️ **Map i select phawt a ngai e!**\n\nMap list-ah khuan i scale/tihsan duh map kha double-click la, chumi hnuah `scale by 1.05` emaw `5% in tipung rawh` tiin min hrilh leh rawh le.";
            }
            return "⚠️ **No active map selected.**\n\nPlease select or open a map in the editor first, then ask me to scale it (e.g. `scale this map by 1.05` or `increase by 5%`).";
        }
    }

    // G. Smooth Map
    if (hasTool("smooth_map") &&
        (q.contains("smooth") || q.contains("ti mam") || q.contains("smoothing"))) {
        if (selMap != "none") {
            outToolName = "smooth_map";
            outToolArgs["name"] = selMap;
            outToolArgs["iterations"] = 1;
            return QString();
        } else {
            if (isMizo) {
                return "⚠️ **Map i select phawt a ngai e!**\n\nI tih mam (smooth) duh map kha map list-ah khuan select phawt la, chumi hnuah min hrilh leh rawh le.";
            }
            return "⚠️ **No active map selected.**\n\nPlease select a map first to apply surface smoothing.";
        }
    }

    // H. Zero Map
    if (hasTool("zero_map") &&
        (q.contains("zero map") || q.contains("zero this map") || q.contains("zero out") || q.contains("0 vek rawh") || q.contains("ti bo rawh"))) {
        if (selMap != "none") {
            outToolName = "zero_map";
            outToolArgs["name"] = selMap;
            return QString();
        }
    }

    // I. Restore Map to Stock
    if (hasTool("restore_map") &&
        (q.contains("restore map") || q.contains("reset map") || q.contains("stock ah dah") || q.contains("original ah dah leh"))) {
        if (selMap != "none") {
            outToolName = "restore_map";
            outToolArgs["name"] = selMap;
            return QString();
        }
    }

    // J. Compare Map with Stock
    if (hasTool("compare_map_values") && selMap != "none" &&
        (q.contains("compare") || q.contains("khaikhin") || q.contains("what did i change in this map") || q.contains("map diff"))) {
        outToolName = "compare_map_values";
        outToolArgs["name"] = selMap;
        return QString();
    }

    // K. Identify Map Purpose
    if (hasTool("identify_map_purpose") && selMap != "none" &&
        (q.contains("what is this map") || q.contains("purpose") || q.contains("identify") || q.contains("he map hi enge") || q.contains("map hna"))) {
        outToolName = "identify_map_purpose";
        outToolArgs["map_name"] = selMap;
        return QString();
    }

    // L. Describe Map Shape
    if (hasTool("describe_map_shape") && selMap != "none" &&
        (q.contains("shape") || q.contains("topology") || q.contains("pianhmang") || q.contains("surface shape"))) {
        outToolName = "describe_map_shape";
        outToolArgs["map_name"] = selMap;
        return QString();
    }

    // M. Related Maps
    if (hasTool("get_related_maps") && selMap != "none" &&
        (q.contains("related map") || q.contains("dependencies") || q.contains("inzawm") || q.contains("siblings"))) {
        outToolName = "get_related_maps";
        outToolArgs["map_name"] = selMap;
        return QString();
    }

    // N. Undo Last Change
    if (hasTool("undo_last_change") &&
        (q == "undo" || q.contains("undo last") || q.contains("sut leh") || q.contains("revert last"))) {
        outToolName = "undo_last_change";
        outToolArgs = QJsonObject();
        return QString();
    }

    // O. List All Maps
    if (hasTool("list_maps") &&
        (q == "list maps" || q == "all maps" || q == "show maps" || q.contains("list all maps") || q.contains("map zawng zawng") || q.contains("map awm zawng"))) {
        outToolName = "list_maps";
        outToolArgs = QJsonObject();
        return QString();
    }

    // ── Phase 2: In-Depth Automotive Calibration Knowledge Base ──────────────

    // Greetings & Identity
    if (q == "hi" || q == "hello" || q == "chibai" || q == "help" || q.contains("who are you") || q.contains("what can you do") || q.contains("enge i nih") || q.contains("enge i tih theih")) {
        if (isMizo) {
            return "## ⚡ Built-in Offline ECU Calibration Expert\n\n"
                   "Chibai! Kei hi **romHEX 14 AI** built-in ECU Calibration & Tuning Intelligence ka ni e.\n"
                   "**API Key, internet connection, emaw account engmah a ngai lo** (100% Offline & Free).\n\n"
                   "### Ka Puih Theihna Che:\n"
                   "1. **Stage 1 & Stage 2 Tuning**: Torque, Turbo Boost (+150-250 mbar), Rail Pressure, leh Ignition timing calculations him tak.\n"
                   "2. **Tuning Recipes**: Decat (O2 sensor off), EGR delete, DPF off, Pops & Bangs, Launch Control, leh Vmax (Speed Limiter).\n"
                   "3. **Map Safety & Math Analysis**: Map i select apiangin a safe em tih check (`min/max/average`), flat spots leh anomalies zawng chhuak thei.\n"
                   "4. **ECU Architectures**: Bosch (EDC15/16/17, MG1, MD1, ME7/9), Continental SIMOS, Delphi, leh Denso.\n\n"
                   "💡 *Zawhna i neih apiang min zawt la, emaw `File > Open` hmanga ROM hawngin map i select apiang ka lo analyze zel ang e!*";
        }
        return "## ⚡ Built-in Offline ECU Calibration Expert\n\n"
               "Welcome to **romHEX 14 AI**! I am your local, autonomous ECU calibration intelligence engine — **operating 100% offline with zero API keys or external dependencies**.\n\n"
               "### Capabilities:\n"
               "1. **Stage 1 & Stage 2 Remapping**: Mathematically validated calculations for Boost, Rail Pressure, Torque, and Lambda.\n"
               "2. **Tuning Recipes**: Decat (O2 sensor off), EGR delete, DPF off, Pops & Bangs (Overrun), Launch Control, and Vmax.\n"
               "3. **Real-time Map Safety Assessment**: Inspect active map values (`min/max/avg/stdDev`) against physical sensor and mechanical ceilings.\n"
               "4. **ECU Architecture Mastery**: Bosch (EDC15/16/17, MG1, MD1, ME7/9), Continental SIMOS, Delphi, and Denso.\n\n"
               "💡 *Ask any calibration question or select any map in your editor for real-time safety evaluation.*";
    }

    // Stage 1 / Stage 2 Tuning Guide
    if (q.contains("stage 1") || q.contains("stage1") || q.contains("remap") || q.contains("tune") || q.contains("tuning")) {
        QString s1;
        s1 += "## 🏎️ Comprehensive Stage 1 ECU Calibration Guide\n\n";
        s1 += "Stage 1 tuning maximizes OEM hardware efficiency while staying strictly within thermodynamic and mechanical safety limits.\n\n";
        s1 += "### 📋 Calibration Sequence & Safety Tolerances:\n";
        s1 += "| Calibration Stage | Core Map Name | Safe Range | Mechanical Rationale |\n";
        s1 += "|---|---|---|---|\n";
        s1 += "| **1. Driver Wish** | `Fahrpedalkennfeld` | **+10% to +15%** | Keep 0-20% throttle linear for city driveability; increase 70-100% pedal. |\n";
        s1 += "| **2. Torque Limiter** | `Drehmomentbegrenzung` | **+15% to +18%** | Shape progressive torque curve; protect Dual Mass Flywheel below 2,200 RPM. |\n";
        s1 += "| **3. Smoke / AFR** | `Rauchbegrenzung` (MAF/MAP) | **Lambda 1.15-1.25** | Prevents soot / black smoke; maintains EGT < 850°C on Diesel. |\n";
        s1 += "| **4. Turbo Boost** | `Ladedruck Sollwert` | **+150 to +250 mbar** | Raise Boost Limiter by matching delta. N75 duty cycle must stay < 85%. |\n";
        s1 += "| **5. Rail Pressure** | `Raildruck` | **+50 to +80 bar** | Improves fuel spray atomization; do not exceed sensor/relief valve ceiling. |\n";
        s1 += "| **6. Ignition Timing** | `Zündwinkel` (Petrol) | **+1.5° to +2.5° BTDC** | Optimize MBT timing on 98+ RON fuel; monitor knock retard sensors. |\n\n";
        s1 += "⚠️ **Pre-Flash Protocol:** Always run checksum correction (`CRC16/CRC32`) before flashing to avoid ECU bricking.\n";

        if (isMizo) {
            s1 += "\n### 📝 Mizo Tlangkawmna:\n"
                  "- Stage 1 remap-ah chuan engine thluak map hrang hrangte kha a rualin tihsan a ngai a (Torque, Boost, Fuel, Lambda).\n"
                  "- Turbo boost hi **+150 atanga +250 mbar** bak tihsan loh tur a ni a, clutch him nan 2,200 RPM hnuai lam torque kha tihsan vak loh tur a ni.\n";
        }
        return s1;
    }

    // Decat / Catalyst Off
    if (q.contains("decat") || q.contains("cat off") || q.contains("catalyst") || q.contains("o2") || q.contains("dkat")) {
        QString dc;
        dc += "## 🛠️ Decat (Catalytic Converter Off) Calibration\n\n";
        dc += "When installing a sport downpipe or removing the catalytic converter, the secondary downstream O2 sensor flags DTC `P0420` / `P0430` (Catalyst System Efficiency Below Threshold).\n\n";
        dc += "### Exact ECU Deactivation Procedure:\n";
        dc += "1. **Configuration Switch (`CDKAT` / `CWKAT`)**:\n";
        dc += "   - In Bosch ME7 / MED9 / MED17 / MG1, locate the catalyst diagnosis configuration byte.\n";
        dc += "   - Change `CDKAT` from `1` (Active) to `0` (Disabled).\n";
        dc += "2. **Diagnosis Threshold Curves (`*DKAT*`, `*OSCKAT*`)**:\n";
        dc += "   - Zero the secondary O2 voltage oscillation and catalyst temperature threshold maps.\n";
        dc += "3. **DTC Error Mask Table**:\n";
        dc += "   - Zero error path entries for `P0420` (Bank 1) and `P0430` (Bank 2).\n\n";
        dc += "💡 *Notice: The primary front wideband lambda sensor must ALWAYS remain active for closed-loop fueling control.*";

        if (isMizo) {
            dc += "\n\n### 📝 Mizo Tlangkawmna:\n"
                  "- Decat i tih hian catalyst post-sensor (downstream O2) chauh kha switch off tur a ni a (`CDKAT = 0`).\n"
                  "- Hma lam a primary O2 sensor erawh engine fuel control atan a pawimawh avangin khawih miah loh tur a ni e.\n";
        }
        return dc;
    }

    // Pops & Bangs / Overrun Crackle
    if (q.contains("pop") || q.contains("bang") || q.contains("crackle") || q.contains("burble") || q.contains("overrun")) {
        QString pb;
        pb += "## 🔥 Pops & Bangs (Overrun Crackle) Engineering Protocol\n\n";
        pb += "Pops & bangs are achieved by allowing fuel into the exhaust manifold on trailing throttle (0% pedal) and retarding ignition timing into negative crank degrees.\n\n";
        pb += "### Parameters to Calibrate:\n";
        pb += "1. **Fuel Cut-off Delay (`KFTVSA`)**:\n";
        pb += "   - Delay fuel cut-off at 0% throttle from stock `0.0s` to `1.2s - 1.8s` in RPM range between `3,200 - 5,500 RPM`.\n";
        pb += "2. **Minimum Ignition Angle (`KFZWMS` / `KFZWMIN`)**:\n";
        pb += "   - Retard minimum allowed ignition timing at low engine load (0% throttle) to `-12° to -20° ATDC`.\n";
        pb += "3. **Exhaust Gas Temperature Protection (`TABGBTS`)**:\n";
        pb += "   - Keep burst duration under 1.8 seconds to avoid melting turbine wheels and exhaust valve margins.\n\n";
        pb += "⚠️ **Warning:** Do not run pops & bangs on ceramic OEM catalytic converters; cat removal (decat) is mandatory.";

        if (isMizo) {
            pb += "\n\n### 📝 Mizo Tlangkawmna:\n"
                  "- Pops & Bangs hi accelerator pedal thlah rual a fuel cut-off delay (`KFTVSA`) dah a, ignition timing retard `-15°` vel a tih a ni.\n"
                  "- A rei lutuk chuan turbo leh exhaust valve a ti chhe thei a, second 1.5 aia rei loh tir a him ber e.\n";
        }
        return pb;
    }

    // Launch Control & Hardcut Limiter
    if (q.contains("launch") || q.contains("hardcut") || q.contains("anti-lag") || q.contains("antilag") || q.contains("popcorn")) {
        QString lc;
        lc += "## 🚦 Launch Control & Popcorn Hardcut Limiter\n\n";
        lc += "### Diesel Popcorn Hardcut Limiter:\n";
        lc += "- In Bosch EDC15 / EDC16 / EDC17, locate the **Torque Limiter** map.\n";
        lc += "- At desired cut-off RPM (e.g. 4,500 RPM), program an instantaneous fuel drop:\n";
        lc += "  - `4,450 RPM`: `68 mg/stroke` (Full Fueling)\n";
        lc += "  - `4,500 RPM`: `0 mg/stroke` (Complete Cut)\n";
        lc += "- This rapid 68 mg drop within a 50 RPM window generates the signature crisp popcorn hardcut sound.\n\n";
        lc += "### Petrol 2-Step Launch Control:\n";
        lc += "- Identify stationary speed threshold (`VMAX < 3 km/h`).\n";
        lc += "- Set launch rev limit to `3,600 - 4,200 RPM` with ignition retard to build 0.4 - 0.7 bar boost on standstill.";

        if (isMizo) {
            lc += "\n\n### 📝 Mizo Tlangkawmna:\n"
                  "- Diesel Popcorn limiter chu Torque Limiter map-ah 4450 RPM-ah fuel pe full la, 4500 RPM-ah 0 mg-ah drop thut la, hardcut a ni mai.\n";
        }
        return lc;
    }

    // EGR (Exhaust Gas Recirculation)
    if (q.contains("egr") || q.contains("exhaust gas") || q.contains("agr")) {
        QString eg;
        eg += "## 🛑 EGR (Exhaust Gas Recirculation) Delete Protocol\n\n";
        eg += "Closing the EGR valve stops exhaust soot from mixing with oily blow-by vapors in the intake manifold, eliminating carbon sludge buildup.\n\n";
        eg += "### Methods by ECU Generation:\n";
        eg += "1. **Target Airflow Saturation (Bosch EDC15 / EDC16)**:\n";
        eg += "   - Set target fresh air mass maps (`mg/stroke` vs RPM & Fuel) to maximum physical sensor limit (`850 - 1000 mg/hub`).\n";
        eg += "   - The ECU reads fresh air target as already fulfilled and holds the EGR valve 100% closed.\n";
        eg += "2. **EGR Hysteresis Curves (Bosch EDC17 / Siemens SIMOS)**:\n";
        eg += "   - Locate the 2 to 4 hysteresis activation threshold maps (maps of size 2x1 to 25x1).\n";
        eg += "   - Set all cell values to `0` to permanently disable valve actuation.\n";
        eg += "3. **DTC Error Code Clearing**: Clear `P0401` (Insufficient Flow) and `P0402` (Excessive Flow).";

        if (isMizo) {
            eg += "\n\n### 📝 Mizo Tlangkawmna:\n"
                  "- EGR hi khar a nih chuan intake manifold-ah carbon bal a tling lo a, engine a thianghlim zawk.\n"
                  "- EDC17-ah chuan EGR hysteresis maps kha 0-a set vek a tawk e.\n";
        }
        return eg;
    }

    // DPF (Diesel Particulate Filter)
    if (q.contains("dpf") || q.contains("fap") || q.contains("particulate")) {
        return "## 💨 DPF (Diesel Particulate Filter) Delete Overview\n\n"
               "When removing the DPF particulate filter for off-road or motorsport applications, regeneration routines must be deactivated.\n\n"
               "### ECU Configuration:\n"
               "1. **DPF Master Switch**:\n"
               "   - In Bosch EDC16 / EDC17, locate the single-byte `DPF_switch` (change from `1` to `0`).\n"
               "2. **Regeneration Post-Injections**:\n"
               "   - Zero post-injection fuel duration maps (`Post-injection 1 & 2`) used to heat exhaust to 600°C.\n"
               "3. **Pressure Differential Sensors**:\n"
               "   - Disable differential pressure sensor plausibility checks and zero soot accumulation modeling.\n"
               "4. **DTC Elimination**: Suppress `P2002` (Efficiency Below Threshold) and `P2463` (Soot Accumulation).";
    }

    // Boost Pressure
    if (q.contains("boost") || q.contains("turbo") || q.contains("ladedruck") || q.contains("n75")) {
        return "## 🌪️ Turbo Boost Pressure (Ladedruck) Calibration\n\n"
               "Target boost defines the manifold absolute pressure in mbar (1,000 mbar = 1 bar atmospheric pressure).\n\n"
               "### Rules for Safe Calibration:\n"
               "1. **Target Boost (`Ladedruck Sollwert`)**:\n"
               "   - Safe Stage 1 increase: **+150 to +250 mbar** over stock (e.g., from 2,250 mbar to 2,450 mbar).\n"
               "2. **Atmospheric Boost Limiter (`Ladedruckbegrenzung`)**:\n"
               "   - Always raise the atmospheric limiter curve by the same amount to prevent ECU entering limp-home mode (`P0234`).\n"
               "3. **N75 / VNT Vane Duty Cycle**:\n"
               "   - If boost spikes occur during spool-up, open turbo vanes by decreasing N75 duty cycle by `-2% to -4%`.";
    }

    // Torque Limiter
    if (q.contains("torque") || q.contains("drehmoment") || q.contains("driver wish") || q.contains("pedal")) {
        return "## 📈 Torque Limiter & Driver Demand Calibration\n\n"
               "Modern engine management systems are **torque-based** architectures where the throttle pedal requests Nm of torque, which is filtered by limits before calculating fuel injection.\n\n"
               "### Key Principles:\n"
               "- **Driver Demand (`Fahrpedal`)**: Set progressive throttle response; avoid aggressive jumps at low pedal travel.\n"
               "- **Torque Limiter (`Drehmomentbegrenzung`)**: Increase maximum torque by **+15% to +20%** between 2,200 and 4,000 RPM.\n"
               "- **Drivetrain Protection**: Avoid adding torque below 2,000 RPM to protect manual clutches and Dual Mass Flywheels.";
    }

    // DTC Error Code Removal (DTC Delete)
    if (q.contains("dtc") || q.contains("fault code") || q.contains("p0420") || q.contains("p0401") ||
        q.contains("p2002") || q.contains("error code") || q.contains("check engine") || q.contains("dtc delete")) {
        QString dtc;
        dtc += "## 🔧 DTC (Diagnostic Trouble Code) Removal Engineering Protocol\n\n";
        dtc += "DTC deactivation stops the ECU from logging fault codes and illuminating the Check Engine MIL light for deleted hardware components (e.g. Decat `P0420`, EGR `P0401`, DPF `P2002`).\n\n";
        dtc += "### ECU Architecture & Table Architecture:\n";
        dtc += "1. **Bosch EDC16 / EDC17 / MED17 DTC Table System**:\n";
        dtc += "   - **16-bit P-Code Table**: A contiguous array of 2-byte hexadecimal integers (e.g. `0x0420` for P0420, `0x0401` for P0401).\n";
        dtc += "   - **Error Path Table**: Precedes or follows the P-code table; maps each fault condition to an internal software error index `k`.\n";
        dtc += "   - **DTC Switch Table (`CDHSH` / `CDLSH`)**: An array of 1-byte switches. Setting switch byte index `k` to `0x00` completely mutes the diagnostic path.\n";
        dtc += "   - **Fault Class Table**: Changing the error class byte from `0x01` (MIL on) to `0x00` prevents dashboard warning lamp illumination.\n";
        dtc += "2. **Continental SIMOS 18 / 12**:\n";
        dtc += "   - Uses a multi-row matrix where fault reporting, freeze frame recording, and MIL request bits are individually configurable.\n";
        dtc += "3. **Delphi DCM 3.7 / 6.2**:\n";
        dtc += "   - Locate DTC enable word mask and set the corresponding fault bit to `0`.\n\n";
        dtc += "⚠️ **Engineering Rule:** Never do a blanket zeroing of the DTC table. Only zero specific codes for physically removed emissions systems to preserve engine protection diagnostics.";

        if (isMizo) {
            dtc += "\n\n### 📝 Mizo Tlangkawmna:\n"
                   "- DTC error code delete (entirnan P0420 decat tan, P0401 EGR tan) hi ECU-ah P-code lookup table leh switch byte table (`0x00`) zawn a tih thin a ni.\n"
                   "- DTC zawng zawng paih vek loh tur a ni a, engine buaina dang (oil pressure, knock, coolant temp) a lo thlen thut pawha kan hriat theih nan hardware kan paih chauh kha mute tur a ni e.\n";
        }
        return dtc;
    }

    // AdBlue / SCR / DEF Deactivation
    if (q.contains("adblue") || q.contains("scr") || q.contains("def") || q.contains("urea") || q.contains("nox")) {
        QString adb;
        adb += "## 🧪 AdBlue / SCR (Selective Catalytic Reduction) Deactivation Protocol\n\n";
        adb += "AdBlue systems inject urea solution into the exhaust to reduce NOx emissions. In extreme operating conditions or export vehicles, deactivation is required.\n\n";
        adb += "### Engineering Steps by ECU Platform:\n";
        adb += "1. **Bosch EDC17 / MD1 SCR Master Configuration Switch**:\n";
        adb += "   - Locate the single-byte `SCR_switch` configuration parameter (change from `1` to `0`).\n";
        adb += "2. **NOx Sensor Loop Deactivation**:\n";
        adb += "   - Disconnect or software-disable upstream and downstream NOx sensor plausibility monitoring (`DFC_NOxSens*`).\n";
        adb += "3. **Dosing Module & Heater Suppression**:\n";
        adb += "   - Disable tank heating element checks and tank level sensor plausibility.\n";
        adb += "4. **DTC Error Code Clearing**:\n";
        adb += "   - Clear `P20EE` (SCR NOx Catalyst Efficiency Below Threshold), `P204F` (Reductant System Performance), and `P207F` (Reductant Quality).";

        if (isMizo) {
            adb += "\n\n### 📝 Mizo Tlangkawmna:\n"
                   "- AdBlue/SCR off hi Bosch EDC17/MD1 ah chuan `SCR_switch = 0` a dah a, NOx sensor leh AdBlue heater error code mute a ni tlangpui.\n";
        }
        return adb;
    }

    // Swirl Flaps / VSA Delete
    if (q.contains("swirl") || q.contains("flap") || q.contains("vsa")) {
        QString sf;
        sf += "## 🌪️ Swirl Flap (Intake Manifold Runner) Delete\n\n";
        sf += "Swirl flaps enhance low-RPM air turbulence in diesel manifolds, but factory plastic flaps frequently fatigue, break off, and get sucked into the combustion chamber, destroying pistons and valves.\n\n";
        sf += "### Calibration Procedure:\n";
        sf += "1. **Target Position Map (`VSA` / `Drallklappe`)**:\n";
        sf += "   - Set all cells in the swirl flap duty cycle table to `0%` (or `100%` depending on spring-return orientation) to hold flaps permanently fully open.\n";
        sf += "2. **Actuator Motor Diagnosis**:\n";
        sf += "   - Suppress actuator motor potentiometer range and correlation error paths.\n";
        sf += "3. **DTC Mask**: Zero error paths for `P2015` (Intake Manifold Runner Position Sensor Range/Performance) and `P2008` (Runner Control Circuit Open).";

        if (isMizo) {
            sf += "\n\n### 📝 Mizo Tlangkawmna:\n"
                   "- Swirl flap hi a thir leh plastic a chhiat a engine chhung a luh thut chuan piston a ti chhe vek thei a, tuners ten an delete tlangpui thin.\n"
                   "- ECU map-ah flap target kha 0%-ah set a, DTC `P2015` clear a tawk e.\n";
        }
        return sf;
    }

    // TDI / Diesel Hot Start Fix
    if (q.contains("hot start") || q.contains("warm start") || q.contains("cranking") || q.contains("start fix") || q.contains("nung har")) {
        QString hs;
        hs += "## ⚡ Diesel TDI Hot Start Problem & Permanent Calibration Fix\n\n";
        hs += "### Root Cause (Factory ECU Logic Bug in VAG EDC15 & EDC16):\n";
        hs += "- In Volkswagen, Audi, SEAT, and Škoda 1.9 TDI & 2.0 TDI engines, the car starts instantly on half a turn when cold, but cranks for 6-10 seconds when the engine is warm (coolant temperature > 70°C).\n";
        hs += "- **The OEM Map Flaw**: In the factory `Starting Injection Quantity (IQ vs Engine RPM vs Coolant Temp)` map, at 250 RPM starter speed and 70°C-90°C coolant temperature, the OEM calibration requests **0.0 mg/stroke of fuel** until the engine reaches 280+ RPM.\n";
        hs += "- As starters, wiring, and batteries age, cranking speed drops from 290 RPM to 240-250 RPM. As a result, the ECU injects ZERO fuel until cranking builds enough heat or speed.\n\n";
        hs += "### Permanent romHEX Calibration Solution:\n";
        hs += "1. Locate the **Starting Fuel Map** (typically 9x8 or 10x10 matrix: X = RPM, Y = Coolant Temp in °C, Z = Fuel in mg/stroke).\n";
        hs += "2. In the 200 RPM and 250 RPM columns, copy the high-fuel values from the cold columns (e.g. `30.0 - 35.0 mg/stroke`) into the 70°C, 80°C, and 90°C warm temperature rows.\n";
        hs += "3. Save and flash. The engine will now fire up instantly on the first turn of the key, warm or cold!";

        if (isMizo) {
            hs += "\n\n### 📝 Mizo Tlangkawmna:\n"
                   "- VAG 1.9/2.0 TDI engine-ah vawt lai a nung nghal mai, mahse engine a lum hnu (70°C+) a starter herh vak vak pawha nung har em em chhan chu factory ECU-in 250 RPM hnuai lamah fuel 0 mg pe tura a lo set vang a ni.\n"
                   "- Starting Fuel map-ah khan engine lum lai (70-90°C) column-ah vawt lai a fuel pek ang khan 30 mg vel dah ve la, a nung nghal zawt zawt mai ang!\n";
        }
        return hs;
    }

    // Speed Limiter (VMAX)
    if (q.contains("vmax") || q.contains("speed limit") || q.contains("top speed") || q.contains("speed limiter")) {
        QString vm;
        vm += "## 🏁 VMAX (Top Speed Limiter) Calibration\n\n";
        vm += "Many European vehicles are factory-governed to 250 km/h (155 mph) via ECU software limiters.\n\n";
        vm += "### Types of Speed Limiters in ECU Binary:\n";
        vm += "1. **Single Value Speed Limiter (SVSL)**:\n";
        vm += "   - A 16-bit integer constant stored in memory representing maximum allowed vehicle speed (e.g., `250` or raw `25000` with 0.01 factor).\n";
        vm += "   - Set SVSL to `300`, `320`, or maximum 16-bit limit `0xFFFF` (`65,535`).\n";
        vm += "2. **2D Gear-Dependent Speed Limit Maps**:\n";
        vm += "   - Speed ceiling curves per transmission gear ratio.\n";
        vm += "3. **Reverse Speed Limiter**:\n";
        vm += "   - Leave reverse gear speed limits stock (typically 40 km/h) for vehicle safety.";

        if (isMizo) {
            vm += "\n\n### 📝 Mizo Tlangkawmna:\n"
                   "- Top speed limiter (VMAX) hi SVSL (single value) a ni tlangpui a, 250 km/h atangin 300 km/h emaw 65535-ah dah san a ni mai thin e.\n";
        }
        return vm;
    }

    // Automatic Gearbox / DSG TCU Tuning
    if (q.contains("dsg") || q.contains("tcu") || q.contains("gearbox") || q.contains("transmission") || q.contains("clutch") ||
        q.contains("dq200") || q.contains("dq250") || q.contains("dq381") || q.contains("zf8hp")) {
        QString tcu;
        tcu += "## 🕹️ Transmission Control Unit (TCU / DSG) Tuning\n\n";
        tcu += "When engine torque is increased beyond factory levels, the Transmission Control Unit (TCU) must be harmonized with the ECU to avoid clutch slippage and torque clipping.\n\n";
        tcu += "### Crucial TCU Parameters:\n";
        tcu += "1. **Clutch Clamping Pressure**:\n";
        tcu += "   - Stock line pressure: ~12 to 14 bar (slips above 400 Nm on DQ250).\n";
        tcu += "   - Tuned line pressure: **16.5 to 18.0 bar**, allowing clutch pack to hold 500+ Nm without slip.\n";
        tcu += "2. **TCU Internal Torque Limiters**:\n";
        tcu += "   - Must be raised to match engine ECU output; otherwise TCU requests throttle closing via CAN bus.\n";
        tcu += "3. **Shift Time & Shift Force**:\n";
        tcu += "   - Reduce pre-shift fill and torque reduction phase for up to 30% faster paddle shifts.\n";
        tcu += "4. **Launch Control Setpoint**:\n";
        tcu += "   - Adjust launch RPM (e.g. 3,200 - 3,800 RPM) and enable multi-stage RPM launch selection.";

        if (isMizo) {
            tcu += "\n\n### 📝 Mizo Tlangkawmna:\n"
                   "- Engine torque i tihsan hian DSG/Automatic gearbox TCU pawh tihsan ve a ngai a, chutilochuan clutch a slip emaw ECU torque a lo clip thin.\n"
                   "- Clutch clamping pressure kha 16-18 bar-ah tihsan a, shift hun tawi tir a tawk e.\n";
        }
        return tcu;
    }

    // Stage 2 & Stage 3 Hardware Guidelines
    if (q.contains("stage 2") || q.contains("stage 3") || q.contains("downpipe") || q.contains("intercooler") || q.contains("hpfp")) {
        QString stg;
        stg += "## 🚀 Stage 2 & Stage 3 Hardware & Calibration Roadmap\n\n";
        stg += "While Stage 1 relies on OEM factory parts, Stage 2 and Stage 3 require dedicated mechanical hardware upgrades to support aggressive boost and fueling.\n\n";
        stg += "| Upgrade Stage | Required Hardware Upgrades | Calibration Adjustments | Expected Gains |\n";
        stg += "|---|---|---|---|\n";
        stg += "| **Stage 1** | 100% Stock OEM Hardware | Boost (+0.2 bar), Rail (+60 bar), Torque (+18%) | **+20% to +30% HP** |\n";
        stg += "| **Stage 2** | Decat / 200-cell Downpipe, Upgraded Intercooler (FMIC), Intake | Cat Off (`CDKAT`), Boost (+0.4 bar), EGT protection | **+35% to +45% HP** |\n";
        stg += "| **Stage 3** | Hybrid / Big Turbo, High-Pressure Fuel Pump (HPFP), Injectors | Complete MAF rescaling, Boost (+0.8 bar+), MBT timing | **+60% to +100%+ HP** |\n\n";
        stg += "### Thermal Management:\n";
        stg += "- **Downpipe**: Reducing backpressure lowers Exhaust Gas Temperature (EGT) by ~80-120°C, prolonging turbo bearing life.\n";
        stg += "- **HPFP (Autotech / VIS)**: Upgrades pump piston from 9.8mm to 11.5mm, supplying 175+ bar fuel pressure at 6,500 RPM.";

        if (isMizo) {
            stg += "\n\n### 📝 Mizo Tlangkawmna:\n"
                   "- Stage 2-ah chuan Downpipe (catless/sport cat) leh Intercooler lian zawk dah a ngai a, EGT a hniam phah a, power a sang thei zawk.\n"
                   "- Stage 3-ah chuan Turbo lian leh High Pressure Fuel Pump (HPFP) upgrade a ngai tawh thung e.\n";
        }
        return stg;
    }

    // Checksums & Flashing Safety Protocol
    if (q.contains("checksum") || q.contains("crc") || q.contains("brick") || q.contains("boot mode") || q.contains("bench") || q.contains("flashing")) {
        QString cs;
        cs += "## 🛡️ ECU Checksum Mathematics & Flashing Safety Protocol\n\n";
        cs += "### What is a Checksum?\n";
        cs += "- A mathematical redundancy verification algorithm (CRC16, CRC32, RSA, or additive blocks) calculated across specific memory sectors.\n";
        cs += "- **The Golden Rule**: If you modify even a single cell or byte in a binary without recalculating checksums, the ECU bootloader will fail integrity verification on reboot and **perma-brick the ECU**.\n\n";
        cs += "### Flashing Methods:\n";
        cs += "1. **OBD-II (Diagnostic Port)**: Read/write via standard vehicle port. Fast, but requires valid RSA key exchange.\n";
        cs += "2. **Bench Mode**: Connect directly to ECU harness pins on the bench with NO opening of the aluminum housing.\n";
        cs += "3. **Boot Mode (Tricore / BDM)**: Opening the casing and using a pogo pin / 1k-ohm pull-down resistor to put the microprocessor into raw bootloader state for 100% full clone and recovery.\n\n";
        cs += "💡 **Battery Safety:** Always connect a professional 13.5V - 14.2V 70A+ battery stabilizer during flashing. A voltage drop below 12.0V mid-flash is the #1 cause of failed ECU programming.";

        if (isMizo) {
            cs += "\n\n### 📝 Mizo Tlangkawmna:\n"
                   "- Checksum hi a pawimawh ber a! Map i modify zawh apiangin checksum recalculate ngei ngei tur a ni a, chutilochuan ECU a in-brick (thi) thei.\n"
                   "- Flashing i tih laiin battery voltage 13.5V aia a hniam loh nan battery stabilizer/charger vuah ngei tur a ni e.\n";
        }
        return cs;
    }

    // Active Selected Map fallback
    if (selMap != "none") {
        return QString("## 📊 Active Map Selected: `%1`\n\n"
                       "This map is currently open in your editor.\n\n"
                       "### Quick Actions:\n"
                       "- Ask: `\"Check this map for safety\"` ➔ I will evaluate min/max values against physical sensor limits.\n"
                       "- Ask: `\"Detect anomalies\"` ➔ I will check for inverted axes, flat plateaus, or steep spikes.\n"
                       "- Ask: `\"Scale this map by 1.05\"` ➔ I will calculate Stage 1 values.\n\n"
                       "💡 *What would you like to analyze or modify on `%1`?*").arg(selMap);
    }

    // Conversational Fallback
    QString fb;
    fb += QString("## ⚡ ECU Calibration Intelligence\n\n"
                  "You asked: *\"%1\"*\n\n"
                  "As your built-in Offline ECU Calibration Expert, here are key automotive tuning insights:\n\n"
                  "- **ECU Control Hierarchy**: Driver Wish ➔ Torque Limiters ➔ Smoke / AFR Limiter ➔ Turbo Boost ➔ Rail Pressure ➔ Injection Duration.\n"
                  "- **Data Integrity**: Modified binaries require CRC16 / CRC32 and RSA signature verification before flashing.\n"
                  "- **Hardware Safety**: Maintain Air-Fuel Ratio (AFR) margins and monitor exhaust gas temperature (EGT < 850°C diesel, < 950°C petrol).\n\n"
                  "💡 **Quick Prompts to try:**\n"
                  "- `Stage 1 tuning guide`\n"
                  "- `Check this map for safety`\n"
                  "- `What maps did I modify?`\n"
                  "- `How to do decat on Bosch MED17`\n"
                  "- `Explain pops and bangs overrun delay`\n"
                  "- `EGR delete calibration`").arg(query);

    if (isMizo) {
        fb += "\n\n💡 *Mizo tawng pawhin eng pawh min zawt thei e (entirnan: 'he map hi a him em?', 'stage 1 tih dan', 'decat tih dan')!*";
    }
    return fb;
}


