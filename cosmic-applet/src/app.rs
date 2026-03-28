use cosmic::app::{Core, Task};
use cosmic::iced::window::Id;
use cosmic::prelude::*;
use cosmic::widget;
use std::process::Command;

const APP_ID: &str = "com.screenshot.CosmicApplet";
const ICON: &str = "camera-photo-symbolic";

#[derive(Default)]
pub struct AppModel {
    core: Core,
    is_busy: bool,
    last_error: Option<String>,
}

#[derive(Debug, Clone)]
pub enum Message {
    Capture,
    Spawned(Result<(), String>),
    Surface(cosmic::surface::Action),
}

fn screenshot_binary() -> String {
    std::env::var("SCREENSHOT_BIN").unwrap_or_else(|_| "screenshot".to_string())
}

impl cosmic::Application for AppModel {
    type Executor = cosmic::SingleThreadExecutor;
    type Flags = ();
    type Message = Message;

    const APP_ID: &'static str = APP_ID;

    fn core(&self) -> &Core {
        &self.core
    }

    fn core_mut(&mut self) -> &mut Core {
        &mut self.core
    }

    fn init(core: Core, _flags: Self::Flags) -> (Self, Task<Self::Message>) {
        (
            Self {
                core,
                ..Default::default()
            },
            Task::none(),
        )
    }

    fn view(&self) -> Element<'_, Self::Message> {
        let tooltip = if self.is_busy {
            "Starting screenshot…".to_owned()
        } else if let Some(err) = &self.last_error {
            err.clone()
        } else {
            "Region screenshot".to_owned()
        };

        let button = self
            .core
            .applet
            .icon_button(ICON)
            .on_press(Message::Capture);

        Element::from(
            self.core
                .applet
                .applet_tooltip(button, tooltip, false, Message::Surface, None),
        )
    }

    fn view_window(&self, _id: Id) -> Element<'_, Self::Message> {
        widget::text("Screenshot").into()
    }

    fn update(&mut self, message: Self::Message) -> Task<Self::Message> {
        match message {
            Message::Surface(action) => cosmic::task::message(cosmic::Action::Cosmic(
                cosmic::app::Action::Surface(action),
            )),
            Message::Capture => {
                if self.is_busy {
                    return Task::none();
                }
                self.is_busy = true;
                self.last_error = None;
                let bin = screenshot_binary();
                Task::perform(
                    async move {
                        match Command::new(&bin)
                            .stdin(std::process::Stdio::null())
                            .stdout(std::process::Stdio::null())
                            .stderr(std::process::Stdio::null())
                            .status()
                        {
                            Ok(s) if s.success() => Ok(()),
                            Ok(s) => Err(match s.code() {
                                Some(c) => format!("screenshot exited with code {c}"),
                                None => "screenshot terminated by signal".to_string(),
                            }),
                            Err(e) => Err(format!("Could not run {bin}: {e}")),
                        }
                    },
                    |result| cosmic::Action::App(Message::Spawned(result)),
                )
            }
            Message::Spawned(result) => {
                self.is_busy = false;
                match result {
                    Ok(()) => {
                        self.last_error = None;
                    }
                    Err(err) => {
                        tracing::warn!("screenshot run failed: {err}");
                        self.last_error = Some(err);
                    }
                }
                Task::none()
            }
        }
    }

    fn style(&self) -> Option<cosmic::iced_core::theme::Style> {
        Some(cosmic::applet::style())
    }
}
