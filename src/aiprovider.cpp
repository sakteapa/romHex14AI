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
    for (int i = messages.size() - 1; i >= 0; --i) {
        if (messages[i].role == AIMessage::User && lastUserQuery.isEmpty()) {
            lastUserQuery = messages[i].content;
        } else if (messages[i].role == AIMessage::ToolResult && lastToolResult.isEmpty()) {
            lastToolResult = messages[i].toolResultJson;
        }
    }

    QString toolToCall;
    QJsonObject toolInput;
    QString responseText;

    if (!lastToolResult.isEmpty()) {
        QJsonDocument doc = QJsonDocument::fromJson(lastToolResult.toUtf8());
        if (doc.isObject()) {
            QJsonObject root = doc.object();
            if (root.contains("results") && root["results"].isArray()) {
                QJsonArray resArr = root["results"].toArray();
                if (resArr.isEmpty()) {
                    responseText = "### 🔍 Map Search Result\n\nNo matching maps found for this search pattern in the loaded project.\n\n**Suggestions:**\n- Try searching with broader wildcards (e.g. `*`)\n- Verify if an ASAP2 (A2L) or OLS map pack is loaded for this ROM\n- Check Hex view for table descriptors.";
                } else {
                    responseText = QString("### 🔍 Found %1 Map(s)\n\n| Map Name | Address | Dimensions | Unit |\n|---|---|---|---|\n").arg(resArr.size());
                    for (int j = 0; j < qMin(resArr.size(), 10); ++j) {
                        QJsonObject m = resArr[j].toObject();
                        QString name = m["name"].toString();
                        QString addr = m.contains("address") ? QString("0x%1").arg(m["address"].toInt(), 6, 16, QChar('0')).toUpper() : "N/A";
                        QString dims = (m.contains("rows") && m.contains("cols")) ? QString("%1x%2").arg(m["rows"].toInt()).arg(m["cols"].toInt()) : "N/A";
                        QString unit = m["unit"].toString("-");
                        responseText += QString("| **%1** | `%2` | %3 | %4 |\n").arg(name, addr, dims, unit);
                    }
                    if (resArr.size() > 10) {
                        responseText += QString("\n*...and %1 more maps.*\n").arg(resArr.size() - 10);
                    }
                    responseText += "\n💡 **Tuning Recommendation:**\nDouble-click any map above in the Map List to open its 2D and 3D surface visualizer. Keep Stage 1 increases within safe margins (+5% to +10%).";
                }
            } else {
                responseText = QString("### ⚙️ Operation Completed\n\nResult:\n```json\n%1\n```\n").arg(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)));
            }
        } else {
            responseText = "### ⚙️ Operation Completed\n\n" + lastToolResult;
        }
    } else {
        responseText = generateExpertResponse(lastUserQuery, systemPrompt, tools, toolToCall, toolInput);
    }

    if (!toolToCall.isEmpty() && onToolCall) {
        QTimer::singleShot(40, this, [this, toolToCall, toolInput, onToolCall]() {
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

QString OfflineECUExpertProvider::generateExpertResponse(const QString &query,
                                                         const QString &systemPrompt,
                                                         const QVector<AIToolDef> &tools,
                                                         QString &outToolName,
                                                         QJsonObject &outToolArgs)
{
    QString q = query.trimmed().toLower();

    // Check if tools have search_maps
    bool hasSearchTool = false;
    for (const auto &t : tools) {
        if (t.name == "search_maps") {
            hasSearchTool = true;
            break;
        }
    }

    // 1. Tool-triggering intents
    if (hasSearchTool && (q.contains("search map") || q.contains("find map") || q.startsWith("search ") || q.startsWith("find "))) {
        QString pattern = query;
        pattern.remove(QRegularExpression("^(search|find|locate)\\s+(maps?\\s+)?", QRegularExpression::CaseInsensitiveOption));
        pattern = pattern.trimmed();
        if (!pattern.contains("*")) pattern = "*" + pattern + "*";
        outToolName = "search_maps";
        outToolArgs["query"] = pattern;
        return QString();
    }

    // 2. Greetings / Introduction
    if (q == "hi" || q == "hello" || q == "chibai" || q == "help" || q.contains("who are you") || q.contains("what can you do") || q.contains("enge i tih theih")) {
        return "## ⚡ Built-in Offline ECU Calibration Expert\n\n"
               "Welcome to **romHEX 14 AI**! I am your local, offline ECU calibration intelligence engine — **no API keys, internet connection, or external subscriptions required**.\n\n"
               "### What I can help you with:\n"
               "1. **Stage 1 & Stage 2 Remapping**: Safe torque, boost, rail pressure, and ignition timing calculations.\n"
               "2. **Tuning Recipes**: Decat (O2 sensor off), EGR delete, DPF off, Pops & Bangs (Overrun), Launch Control, and Vmax (Speed Limiter).\n"
               "3. **Map Analysis & Identification**: Explain Bosch (EDC15/16/17, MG1, MD1, ME7/9), Continental SIMOS, Delphi, and Denso maps.\n"
               "4. **ECU Architecture & Calculations**: Scaling factors, axis offsets, air-fuel ratio (AFR / Lambda), and checksum protection.\n\n"
               "💡 *Ask me any calibration question, or open a ROM file (`File > Open`) to inspect and tune your maps!*";
    }

    // 3. Stage 1 Tuning
    if (q.contains("stage 1") || q.contains("stage1") || q.contains("remap") || q.contains("tune")) {
        return "## 🏎️ Stage 1 ECU Calibration Guide\n\n"
               "A successful **Stage 1 Calibration** maximizes factory hardware efficiency without exceeding thermal and mechanical safety limits.\n\n"
               "### Key Map Modifications:\n"
               "| Parameter | Map Name (Bosch / OLS) | Safe Range | Notes |\n"
               "|---|---|---|---|\n"
               "| **Torque Limiter** | `Drehmomentbegrenzung` | **+12% to +18%** | Shape progressive curve; protect dual-mass flywheel below 2,200 RPM. |\n"
               "| **Driver Request** | `Fahrpedalkennfeld` | **+10% to +15%** | Keep 0-20% pedal linear for smooth city driving. |\n"
               "| **Turbo Boost** | `Ladedruck Sollwert` | **+150 to +250 mbar** | Update Boost Limiter map accordingly; verify N75 duty cycle < 85%. |\n"
               "| **Smoke Limiter** | `Rauchbegrenzung` (MAF/MAP) | **Adjust for Lambda 1.15-1.25** | Prevents black smoke / soot under high load on Diesel engines. |\n"
               "| **Rail Pressure** | `Raildruck` | **+50 to +80 bar** | Improves fuel atomization; do not exceed sensor/relief valve ceiling. |\n"
               "| **Ignition Timing** | `Zündwinkel` (Petrol) | **+1.5° to +3.0° BTDC** | Monitor knock sensor retard; optimize MBT without pre-ignition. |\n\n"
               "⚠️ **Safety Rule:** Always verify exhaust gas temperatures (EGT < 850°C on diesel, < 950°C on petrol) and recalculate the ECU checksum before flashing.";
    }

    // 4. Decat (Cat Off / O2 Sensor)
    if (q.contains("decat") || q.contains("cat off") || q.contains("catalyst") || q.contains("o2") || q.contains("lambda sensor")) {
        if (hasSearchTool && (q.contains("apply") || q.contains("do") || q.contains("execute"))) {
            outToolName = "search_maps";
            outToolArgs["query"] = "*KAT*";
            return QString();
        }
        return "## 🛠️ Decat (Catalytic Converter Off) Calibration\n\n"
               "When fitting a decat downpipe or high-flow sport cat, the ECU detects insufficient catalyst efficiency (DTCs `P0420` / `P0430`) via the secondary post-cat oxygen sensor.\n\n"
               "### How to Disable in ECU:\n"
               "1. **Diagnosis Switch (`CDKAT` / `CWKAT`)**:\n"
               "   - In Bosch ME7, MED9, MED17, and MG1, locate the catalyst diagnosis configuration byte.\n"
               "   - Change configuration byte value from `1` to `0`.\n"
               "2. **Diagnosis Characteristic Curves (`*DKAT*`, `*OSCKAT*`)**:\n"
               "   - Set minimum catalyst amplitude/oscillation thresholds to `0` or maximum unreachable limits.\n"
               "3. **DTC Error Code Table**:\n"
               "   - Zero error path entries for `P0420` (Bank 1) and `P0430` (Bank 2) in the DTC table.\n\n"
               "💡 *If you have a ROM project open, you can ask me: `search maps *KAT*` to locate candidate maps!*";
    }

    // 5. Pops & Bangs / Overrun Crackle
    if (q.contains("pop") || q.contains("bang") || q.contains("crackle") || q.contains("burble") || q.contains("overrun")) {
        return "## 🔥 Pops & Bangs (Overrun Crackle) Configuration\n\n"
               "Pops & bangs are achieved by allowing fuel into the exhaust manifold on trailing throttle (0% pedal) and delaying ignition timing into negative crank degrees (ATDC).\n\n"
               "### Implementation Steps:\n"
               "1. **Overrun Fuel Cut-off Delay (`KFTVSA`)**:\n"
               "   - In Bosch ME7/MED9/MED17, locate `KFTVSA` (Delay time for fuel cut-off vs Coolant Temp and RPM).\n"
               "   - Set delay from stock `0.0s` to `1.2s - 2.0s` in RPM range between `3,000 - 5,500 RPM`.\n"
               "2. **Minimum Ignition Angle (`KFZWMS` / `KFZWMIN`)**:\n"
               "   - Retard minimum allowed ignition timing at low engine load (0% throttle) to `-10° to -22° ATDC`.\n"
               "3. **Exhaust Temperature Warning (`TABGBTS`)**:\n"
               "   - Keep durations short (1.5 seconds max) to protect exhaust valves and turbocharger turbine wheels.\n\n"
               "⚠️ **Caution:** Do not use on vehicles fitted with OEM ceramic catalytic converters, as combustion in the exhaust will destroy the honeycomb structure.";
    }

    // 6. Launch Control & Hardcut Limiter
    if (q.contains("launch") || q.contains("hardcut") || q.contains("anti-lag") || q.contains("antilag") || q.contains("popcorn")) {
        return "## 🚦 Launch Control & Popcorn / Hardcut Limiter\n\n"
               "### Diesel Popcorn Limiter:\n"
               "- In Bosch EDC15 / EDC16 / EDC17, modify the **Torque Limiter** at maximum desired RPM (e.g., 4,500 RPM).\n"
               "- At 4,450 RPM set fuel injection to `65 mg/stroke`, and at 4,500 RPM set to `0 mg/stroke`.\n"
               "- The steep drop of 65 mg within 50 RPM produces the crisp 'popcorn' hardcut sound.\n\n"
               "### Petrol Launch Control (Stationary Rev Limiter):\n"
               "- Locate vehicle speed threshold (`VNMAX` / `VMAX`) for stationary condition (usually `V < 3 km/h`).\n"
               "- Configure stationary rev limit to `3,500 - 4,200 RPM` with ignition retard to spool the turbocharger on standstill.";
    }

    // 7. EGR (Exhaust Gas Recirculation)
    if (q.contains("egr") || q.contains("exhaust gas")) {
        if (hasSearchTool && (q.contains("apply") || q.contains("do") || q.contains("execute"))) {
            outToolName = "search_maps";
            outToolArgs["query"] = "*EGR*";
            return QString();
        }
        return "## 🛑 EGR (Exhaust Gas Recirculation) Delete Guide\n\n"
               "Disabling EGR prevents exhaust soot from entering the intake manifold and mixing with oil vapors, which causes carbon buildup.\n\n"
               "### How to Disable:\n"
               "1. **EGR Target Airflow Map (Bosch EDC15/16)**:\n"
               "   - Locate target fresh air mass maps (mg/hub vs RPM and Injected Quantity).\n"
               "   - Set all table cells to the maximum physical sensor limit (e.g. `850 mg/stroke` or `1000 mg/stroke`).\n"
               "   - The ECU sees the airflow target as already satisfied, keeping the EGR valve 100% closed.\n"
               "2. **EGR Hysteresis Curves (Bosch EDC17 / Siemens)**:\n"
               "   - Locate the 2 or 4 hysteresis threshold blocks (maps of size 2x1 to 25x1).\n"
               "   - Zero all hysteresis map entries to shut the valve permanently.\n"
               "3. **DTC Elimination**: Clear fault codes `P0401` / `P0402` if applicable.";
    }

    // 8. DPF (Diesel Particulate Filter)
    if (q.contains("dpf") || q.contains("fap") || q.contains("particulate")) {
        return "## 💨 DPF (Diesel Particulate Filter) Delete Overview\n\n"
               "When removing the DPF filter for competition or off-road use, regeneration routines and differential pressure monitoring must be shut off.\n\n"
               "### ECU Configuration:\n"
               "1. **DPF Master Switch**:\n"
               "   - In Bosch EDC16/EDC17, locate `DPF_switch` byte (set from `1` to `0`).\n"
               "2. **Regeneration Injections**:\n"
               "   - Zero post-injection duration maps (`Post-injection 1 & 2`) used for raising exhaust temperature to 600°C.\n"
               "3. **Sensor Tables**:\n"
               "   - Zero differential pressure sensor linearization and soot accumulation calculation models.\n"
               "4. **DTC Codes**: Disable `P2002` (Efficiency Below Threshold) and `P2463` (Soot Accumulation).";
    }

    // 9. Turbo Boost Pressure
    if (q.contains("boost") || q.contains("turbo") || q.contains("ladedruck") || q.contains("n75")) {
        return "## 🌪️ Turbo Boost Pressure (Ladedruck) Calibration\n\n"
               "Boost pressure maps define the target absolute pressure in mbar (1000 mbar = 1 bar atmospheric pressure).\n\n"
               "### Key Maps to Adjust:\n"
               "1. **Target Boost Pressure (`Ladedruck Sollwert`)**:\n"
               "   - Increase pressure in high load/RPM areas by **+150 to +250 mbar** (e.g. from 2250 mbar stock to 2450 mbar for Stage 1).\n"
               "2. **Boost Pressure Limiter (`Ladedruckbegrenzung`)**:\n"
               "   - Raise atmospheric pressure limiter by the same offset to prevent ECU limp-mode (`P0234`).\n"
               "3. **N75 / VNT Duty Cycle (`Ladedrucksteller`)**:\n"
               "   - Adjust vane position / wastegate duty cycle by **-2% to -4%** at high RPM if boost creep or overboost spikes occur.";
    }

    // 10. Torque Limiter & Driver Request
    if (q.contains("torque") || q.contains("drehmoment") || q.contains("pedal") || q.contains("driver wish")) {
        return "## 📈 Torque Limiter & Driver Wish Calibration\n\n"
               "Modern ECUs are **torque-based architectures** where the accelerator pedal requests torque (Nm), and limiters filter the request before calculating fuel injection.\n\n"
               "### Tuning Principles:\n"
               "- **Driver Wish (`Fahrpedalkennfeld`)**: Convert pedal position % into requested torque. Keep low throttle smooth; increase values above 70% pedal.\n"
               "- **Torque Limiter (`Drehmomentbegrenzung`)**: Increase maximum torque ceiling by **+15% to +20%** between 2,200 and 4,000 RPM.\n"
               "- **Transmission Torque Limit**: On automatic / DSG gearboxes, verify that ECU torque request does not exceed TCU clutch limits (e.g. 350 Nm on DQ200, 500 Nm on DQ250).";
    }

    // 11. Common Rail Pressure
    if (q.contains("rail") || q.contains("common rail") || q.contains("raildruck")) {
        return "## ⛽ Common Rail Pressure Calibration\n\n"
               "Higher rail pressure improves diesel fuel atomization, allowing shorter injection duration and cleaner combustion.\n\n"
               "### Guidelines:\n"
               "- **Stock Ceiling**: 1,600 bar on older generation; 1,800 - 2,200 bar on Piezo / solenoid modern systems.\n"
               "- **Safe Stage 1 Increase**: **+50 to +80 bar** in the upper 80-100% fuel demand zone.\n"
               "- **Safety Check**: Do not exceed mechanical pressure relief valve rating (typically 100 bar above stock max). Update rail pressure limiter maps (`Raildruckbegrenzung`).";
    }

    // 12. Check if user is asking about the currently selected map
    if (systemPrompt.contains("Active map:")) {
        QRegularExpression reMap("Active map:\\s*([^\n\r]+)");
        QRegularExpressionMatch match = reMap.match(systemPrompt);
        if (match.hasMatch()) {
            QString mapName = match.captured(1).trimmed();
            return QString("## 📊 Active Map Analysis: `%1`\n\n"
                           "This map is currently selected in your editor view.\n\n"
                           "### Calibration Recommendations:\n"
                           "- **Axis Inspection**: Ensure axis values (RPM, Load, or Temperature) reflect realistic engine operating states.\n"
                           "- **Modification Strategy**: Use proportional percentage scaling (`* 1.05` for +5%) rather than arbitrary absolute additions.\n"
                           "- **Smoothing**: Inspect the 3D surface plot to avoid sudden spikes or valleys that cause engine hesitation.\n\n"
                           "💡 *Need a specific calculation or modification formula for `%1`? Just ask!*").arg(mapName);
        }
    }

    // 13. Conversational Fallback
    return QString("## ⚡ ECU Calibration Intelligence\n\n"
                   "You asked: *\"%1\"*\n\n"
                   "As your built-in Offline ECU Calibration Expert, here are key automotive tuning insights for this topic:\n\n"
                   "- **ECU Calibration Flow**: Driver Wish ➔ Torque Limiter ➔ Smoke/Lambda Limiter ➔ Boost Target ➔ Rail Pressure ➔ SOI/Duration.\n"
                   "- **Data Integrity**: Any modified binary file must pass CRC16 / CRC32 and RSA checksum verification before flashing.\n"
                   "- **Safety Margins**: Maintain Air-Fuel Ratio (AFR) safety windows and keep exhaust gas temperatures (EGT) within manufacturer limits.\n\n"
                   "💡 **Quick Prompts to try:**\n"
                   "- `Stage 1 tuning guide`\n"
                   "- `How to do decat on Bosch MED17`\n"
                   "- `Explain pops and bangs overrun delay`\n"
                   "- `Safe boost pressure increase for turbo diesel`\n"
                   "- `EGR delete calibration`").arg(query);
}

