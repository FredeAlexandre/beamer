//! The protocol of PROTOCOL.md, spoken over a real loopback connection to an agent whose desktop
//! is a fake that records what it was asked to post.

mod fake;

use fake::Agent;
use machine_agent::{Button, Input, Point};
use serde_json::json;
use std::io::{BufRead, Write};
use std::time::Duration;

#[test]
fn hello_names_the_protocol_the_trust_and_the_displays() {
    let agent = Agent::start();
    let reply = agent.ask(json!({"id": 1, "op": "hello"}));
    assert_eq!(reply["id"], 1);
    assert_eq!(reply["ok"], true);
    assert_eq!(reply["protocol"], 1);
    assert_eq!(reply["agent"], "fake");
    assert_eq!(reply["trusted"], true);
    assert_eq!(
        reply["displays"],
        json!([
            {"x": 0.0, "y": 0.0, "width": 1440.0, "height": 900.0, "main": true},
            {"x": 1440.0, "y": -200.0, "width": 2560.0, "height": 1440.0, "main": false},
        ])
    );
    assert!(reply["mono_ns"].as_u64().is_some());
}

#[test]
fn a_line_that_is_not_json_is_a_bad_request_without_an_id() {
    let agent = Agent::start();
    let reply = agent.ask_line("move the pointer please");
    assert_eq!(reply["id"], serde_json::Value::Null);
    assert_eq!(reply["ok"], false);
    assert_eq!(reply["error"], "bad-request");
    assert!(reply["message"].as_str().is_some_and(|m| !m.is_empty()));
}

#[test]
fn an_unknown_op_is_a_bad_request_and_the_connection_still_serves() {
    let agent = Agent::start();
    let reply = agent.ask(json!({"id": 7, "op": "teleport"}));
    assert_eq!(reply["id"], 7);
    assert_eq!(reply["ok"], false);
    assert_eq!(reply["error"], "bad-request");
    assert_eq!(agent.ask(json!({"id": 8, "op": "hello"}))["ok"], true);
}

#[test]
fn pointer_says_where_the_pointer_is() {
    let agent = Agent::start();
    let reply = agent.ask(json!({"id": 1, "op": "pointer"}));
    assert_eq!(reply["ok"], true);
    assert_eq!(
        (reply["x"].clone(), reply["y"].clone()),
        (json!(100.0), json!(100.0))
    );
}

#[test]
fn move_to_posts_one_move_to_that_point_carrying_the_motion() {
    let agent = Agent::start();
    let reply = agent.ask(json!({"id": 2, "op": "move", "to": [400, 250.5]}));
    assert_eq!(reply["ok"], true);
    assert_eq!(
        (reply["x"].clone(), reply["y"].clone()),
        (json!(400.0), json!(250.5))
    );
    assert_eq!(
        agent.posted(),
        vec![Input::Move {
            to: Point { x: 400.0, y: 250.5 },
            delta: (300.0, 150.5)
        }]
    );
    let reply = agent.ask(json!({"id": 3, "op": "pointer"}));
    assert_eq!(
        (reply["x"].clone(), reply["y"].clone()),
        (json!(400.0), json!(250.5))
    );
}

#[test]
fn move_needs_exactly_one_of_to_and_by_as_two_numbers() {
    let agent = Agent::start();
    for bad in [
        json!({"id": 1, "op": "move"}),
        json!({"id": 1, "op": "move", "to": [1]}),
        json!({"id": 1, "op": "move", "to": ["1", 2]}),
        json!({"id": 1, "op": "move", "to": [1, 2], "by": [1, 2]}),
    ] {
        let reply = agent.ask(bad.clone());
        assert_eq!(reply["error"], "bad-request", "{bad}");
    }
    assert_eq!(agent.posted(), vec![]);
}

#[test]
fn move_by_moves_from_where_the_pointer_is() {
    let agent = Agent::start();
    let reply = agent.ask(json!({"id": 1, "op": "move", "by": [50, -20]}));
    assert_eq!(
        (reply["x"].clone(), reply["y"].clone()),
        (json!(150.0), json!(80.0))
    );
    assert_eq!(
        agent.posted(),
        vec![Input::Move {
            to: Point { x: 150.0, y: 80.0 },
            delta: (50.0, -20.0)
        }]
    );
}

#[test]
fn a_push_past_an_edge_holds_the_pointer_at_the_edge_and_carries_the_whole_push() {
    let agent = Agent::start();
    let reply = agent.ask(json!({"id": 1, "op": "move", "by": [-150, 0]}));
    assert_eq!(
        (reply["x"].clone(), reply["y"].clone()),
        (json!(0.0), json!(100.0))
    );
    agent.ask(json!({"id": 2, "op": "move", "by": [0, 900]}));
    assert_eq!(
        agent.posted(),
        vec![
            Input::Move {
                to: Point { x: 0.0, y: 100.0 },
                delta: (-150.0, 0.0)
            },
            Input::Move {
                to: Point { x: 0.0, y: 899.0 },
                delta: (0.0, 900.0)
            },
        ]
    );
}

#[test]
fn a_move_onto_another_display_crosses_to_it() {
    let agent = Agent::start();
    agent.ask(json!({"id": 1, "op": "move", "to": [1400, 10]}));
    let reply = agent.ask(json!({"id": 2, "op": "move", "by": [100, -100]}));
    assert_eq!(
        (reply["x"].clone(), reply["y"].clone()),
        (json!(1500.0), json!(-90.0))
    );
}

#[test]
fn a_click_is_a_press_then_a_release_of_one_button() {
    let agent = Agent::start();
    for (id, request) in [
        json!({"op": "button", "button": "left", "action": "click"}),
        json!({"op": "button", "button": "right", "action": "down"}),
        json!({"op": "button", "button": "middle", "action": "up"}),
    ]
    .into_iter()
    .enumerate()
    {
        let mut request = request;
        request["id"] = json!(id);
        assert_eq!(agent.ask(request)["ok"], true);
    }
    assert_eq!(
        agent.posted(),
        vec![
            Input::Button {
                button: Button::Left,
                down: true
            },
            Input::Button {
                button: Button::Left,
                down: false
            },
            Input::Button {
                button: Button::Right,
                down: true
            },
            Input::Button {
                button: Button::Middle,
                down: false
            },
        ]
    );
}

#[test]
fn keys_are_named_by_their_physical_position() {
    let agent = Agent::start();
    assert_eq!(
        agent.ask(json!({"id": 1, "op": "key", "key": "KeyQ", "action": "press"}))["ok"],
        true
    );
    assert_eq!(
        agent.ask(json!({"id": 2, "op": "key", "key": "ShiftLeft", "action": "down"}))["ok"],
        true
    );
    let posted = agent.posted();
    let keys: Vec<_> = posted
        .iter()
        .map(|i| match i {
            Input::Key { key, down } => (key.code, key.mac, *down),
            other => panic!("not a key: {other:?}"),
        })
        .collect();
    assert_eq!(
        keys,
        vec![
            ("KeyQ", 0x0C, true),
            ("KeyQ", 0x0C, false),
            ("ShiftLeft", 0x38, true)
        ]
    );
}

#[test]
fn a_key_the_protocol_does_not_know_is_refused() {
    let agent = Agent::start();
    let reply = agent.ask(json!({"id": 1, "op": "key", "key": "a", "action": "press"}));
    assert_eq!(reply["error"], "unknown-key");
    let reply = agent.ask(json!({"id": 2, "op": "key-state", "key": "Hyper"}));
    assert_eq!(reply["error"], "unknown-key");
    assert_eq!(agent.posted(), vec![]);
}

#[test]
fn an_action_other_than_down_up_or_its_pair_is_a_bad_request() {
    let agent = Agent::start();
    for request in [
        json!({"id": 1, "op": "key", "key": "KeyA", "action": "click"}),
        json!({"id": 1, "op": "key", "key": "KeyA"}),
        json!({"id": 1, "op": "button", "button": "left", "action": "press"}),
        json!({"id": 1, "op": "button", "button": "back", "action": "click"}),
    ] {
        assert_eq!(
            agent.ask(request.clone())["error"],
            "bad-request",
            "{request}"
        );
    }
    assert_eq!(agent.posted(), vec![]);
}

#[test]
fn key_state_says_whether_the_os_sees_a_key_held() {
    let agent = Agent::start();
    agent.ask(json!({"id": 1, "op": "key", "key": "F19", "action": "down"}));
    assert_eq!(
        agent.ask(json!({"id": 2, "op": "key-state", "key": "F19"}))["down"],
        true
    );
    agent.ask(json!({"id": 3, "op": "key", "key": "F19", "action": "up"}));
    assert_eq!(
        agent.ask(json!({"id": 4, "op": "key-state", "key": "F19"}))["down"],
        false
    );
}

#[test]
fn scroll_turns_the_wheel_by_whole_lines() {
    let agent = Agent::start();
    assert_eq!(
        agent.ask(json!({"id": 1, "op": "scroll", "dy": -3}))["ok"],
        true
    );
    assert_eq!(
        agent.ask(json!({"id": 2, "op": "scroll", "dx": 2, "dy": 1}))["ok"],
        true
    );
    assert_eq!(
        agent.ask(json!({"id": 3, "op": "scroll", "dy": 1.5}))["error"],
        "bad-request"
    );
    assert_eq!(
        agent.ask(json!({"id": 4, "op": "scroll"}))["error"],
        "bad-request"
    );
    assert_eq!(
        agent.posted(),
        vec![
            Input::Scroll { dx: 0, dy: -3 },
            Input::Scroll { dx: 2, dy: 1 }
        ]
    );
}

#[test]
fn clock_reads_both_clocks_and_any_id_comes_back_as_sent() {
    let agent = Agent::start();
    let reply = agent.ask(json!({"id": "pair-17", "op": "clock"}));
    assert_eq!(reply["id"], "pair-17");
    assert_eq!(reply["mono_ns"], 1_000);
    let unix_ns = reply["unix_ns"].as_u64().unwrap();
    assert!(
        unix_ns > 1_700_000_000_000_000_000,
        "{unix_ns} is not a Unix time in ns"
    );
}

#[test]
fn without_the_os_grant_input_is_refused_and_nothing_is_posted() {
    let agent = Agent::start_with(false);
    assert_eq!(agent.ask(json!({"id": 1, "op": "hello"}))["trusted"], false);
    let reply = agent.ask(json!({"id": 2, "op": "move", "by": [1, 1]}));
    assert_eq!(reply["ok"], false);
    assert_eq!(reply["error"], "not-trusted");
    assert_eq!(agent.ask(json!({"id": 3, "op": "pointer"}))["ok"], true);
    assert_eq!(agent.posted(), vec![]);
}

#[test]
fn two_connections_are_served_at_once() {
    let agent = Agent::start();
    let (mut first_reader, mut first) = fake::connect(agent.port);
    // The first connection stays open and idle while another one is served.
    assert_eq!(agent.ask(json!({"id": 1, "op": "hello"}))["ok"], true);
    first
        .write_all(b"{\"id\": 2, \"op\": \"pointer\"}\n")
        .unwrap();
    let mut line = String::new();
    first_reader.read_line(&mut line).unwrap();
    assert!(line.contains("\"ok\":true"), "{line}");
}

#[test]
fn quit_answers_then_stops_the_agent() {
    let agent = Agent::start();
    assert_eq!(agent.ask(json!({"id": 1, "op": "quit"}))["ok"], true);
    assert!(agent.stops_within(Duration::from_secs(2)));
}

#[test]
fn an_agent_left_without_requests_stops_by_itself() {
    let agent = Agent::start_idle(Duration::from_millis(300));
    assert_eq!(agent.ask(json!({"id": 1, "op": "hello"}))["ok"], true);
    assert!(!agent.stops_within(Duration::from_millis(150)));
    assert!(agent.stops_within(Duration::from_secs(2)));
}
