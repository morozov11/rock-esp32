use serde_json::Value;
use super::protocol::Envelope;

pub trait CommandHandler {
    fn capability(&self) -> &'static str;
    fn handle(&mut self, body: &Value) -> Result<Value, &'static str>;
}

pub struct Dispatcher<'a> {
    handlers: &'a mut [&'a mut dyn CommandHandler],
}

impl<'a> Dispatcher<'a> {
    pub fn new(handlers: &'a mut [&'a mut dyn CommandHandler]) -> Self {
        Self { handlers }
    }
    pub fn dispatch(&mut self, frame: &Envelope) -> Result<Value, &'static str> {
        if frame.kind != "device.command" {
            return Err("unsupported_message_type");
        }
        let body = frame.payload.get("body").ok_or("invalid_command")?;
        let name = body
            .get("name")
            .and_then(Value::as_str)
            .ok_or("invalid_command")?;
        self.handlers
            .iter_mut()
            .find(|h| name.starts_with(h.capability()))
            .ok_or("capability_not_supported")?
            .handle(body)
    }
}
