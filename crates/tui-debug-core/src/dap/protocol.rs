use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;

/// Outgoing DAP request envelope.
#[derive(Debug, Serialize)]
pub struct Request {
    pub seq: i64,
    #[serde(rename = "type")]
    pub message_type: &'static str,
    pub command: &'static str,
    pub arguments: Value,
}

/// Parsed inbound DAP message (response or event).
#[derive(Debug, Clone)]
pub enum InboundMessage {
    Response {
        request_seq: i64,
        success: bool,
        command: String,
        body: Value,
        message: Option<String>,
    },
    Event {
        event: String,
        body: Value,
    },
}

impl InboundMessage {
    pub fn parse(value: Value) -> Result<Self> {
        let message_type = value
            .get("type")
            .and_then(Value::as_str)
            .context("DAP message missing type field")?;

        match message_type {
            "response" => Ok(Self::Response {
                request_seq: value
                    .get("request_seq")
                    .and_then(Value::as_i64)
                    .context("response missing request_seq")?,
                success: value
                    .get("success")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
                command: value
                    .get("command")
                    .and_then(Value::as_str)
                    .unwrap_or("")
                    .to_string(),
                body: value.get("body").cloned().unwrap_or(Value::Null),
                message: value
                    .get("message")
                    .and_then(Value::as_str)
                    .map(str::to_string),
            }),
            "event" => Ok(Self::Event {
                event: value
                    .get("event")
                    .and_then(Value::as_str)
                    .context("event missing event name")?
                    .to_string(),
                body: value.get("body").cloned().unwrap_or(Value::Null),
            }),
            other => anyhow::bail!("unknown DAP message type: {other}"),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Thread {
    pub id: i64,
    pub name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StackFrame {
    pub id: i64,
    pub name: String,
    pub line: i64,
    pub column: i64,
    pub source: Option<Source>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Source {
    pub name: Option<String>,
    pub path: Option<String>,
    #[serde(rename = "sourceReference")]
    pub source_reference: Option<i64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Scope {
    pub name: String,
    #[serde(rename = "variablesReference")]
    pub variables_reference: i64,
    pub expensive: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Variable {
    pub name: String,
    pub value: String,
    #[serde(rename = "type")]
    pub type_name: Option<String>,
    #[serde(rename = "variablesReference")]
    pub variables_reference: i64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StoppedEventBody {
    #[serde(rename = "threadId")]
    pub thread_id: i64,
    pub reason: String,
    pub description: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct OutputEventBody {
    pub category: Option<String>,
    pub output: String,
}
