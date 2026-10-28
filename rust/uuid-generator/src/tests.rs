// Copyright (c) 2026 OceanBase.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use super::*;
use std::collections::HashSet;

#[test]
fn prefers_mac_without_generating_random_node() {
    let mac = [2, 3, 4, 5, 6, 7];
    assert_eq!(
        select_node(Some(mac), |_| panic!("MAC should win")).unwrap(),
        mac
    );
}

#[test]
fn absent_or_unusable_mac_uses_random_node() {
    for mac in [
        None,
        Some([0; 6]),
        Some([255; 6]),
        Some([1, 2, 3, 4, 5, 6]),
        Some([2, 0, 0, 0, 0, 0]),
    ] {
        let node = select_node(mac, |output| {
            output.copy_from_slice(&[0x10, 2, 3, 4, 5, 6]);
            Ok(())
        })
        .unwrap();
        assert_eq!(node, [0x11, 2, 3, 4, 5, 6]);
    }
}

#[test]
fn entropy_failure_is_reported() {
    assert_eq!(
        select_node(None, |_| Err(Code::Entropy)),
        Err(Code::Entropy)
    );
}

#[test]
fn v1_layout_and_timestamp_match_standard_vector() {
    let node = [0x9f, 0x6b, 0xde, 0xce, 0xd8, 0x46];
    let uuid = Uuid::new_v1(
        Timestamp::from_gregorian_time(0x1ec9414c232ab00, 0x33c8),
        &node,
    );
    assert_eq!(uuid.to_string(), "c232ab00-9414-11ec-b3c8-9f6bdeced846");
    assert_eq!(uuid.as_bytes()[6] >> 4, 1);
    assert_eq!(uuid.as_bytes()[8] >> 6, 2);
    assert_eq!(uuid.get_node_id(), Some(node));
}

#[test]
fn shared_context_distinguishes_same_time_and_clock_rollback() {
    let generator = Generator {
        node: [2, 3, 4, 5, 6, 7],
        context: ContextV1::new(123),
    };
    let mut seen = HashSet::new();
    for _ in 0..10_000 {
        assert!(seen.insert(generator.at(1_700_000_000, 0)));
    }
    for _ in 0..100 {
        assert!(seen.insert(generator.at(1_699_999_999, 0)));
    }
}

#[test]
fn random_node_is_stable_and_generation_works_without_interfaces() {
    let node = select_node(None, random_bytes).unwrap();
    assert_eq!(node[0] & 1, 1);
    let generator = Generator {
        node,
        context: ContextV1::new(123),
    };
    for _ in 0..100 {
        let uuid = generator.generate().unwrap();
        assert_eq!(uuid.get_node_id(), Some(node));
        assert_eq!(uuid.get_version_num(), 1);
    }
}

#[test]
fn concurrent_lazy_initialization_and_generation_are_unique() {
    let workers: Vec<_> = (0..8)
        .map(|_| {
            std::thread::spawn(|| {
                assert_eq!(bridge::init(), Code::Ok);
                (0..10_000)
                    .map(|_| {
                        let mut bytes = [0; 16];
                        assert_eq!(bridge::generate(&mut bytes), Code::Ok);
                        bytes
                    })
                    .collect::<Vec<_>>()
            })
        })
        .collect();
    let mut seen = HashSet::new();
    let mut node = None;
    for worker in workers {
        for bytes in worker.join().unwrap() {
            let uuid = Uuid::from_bytes(bytes);
            assert_eq!(uuid.get_version_num(), 1);
            assert_eq!(uuid.get_variant(), uuid::Variant::RFC4122);
            assert_eq!(*node.get_or_insert(uuid.get_node_id()), uuid.get_node_id());
            assert!(seen.insert(bytes));
        }
    }
    assert_eq!(seen.len(), 80_000);
}

#[test]
fn bridge_rejects_bad_buffers_without_writing() {
    for length in [0, 15, 17, 36] {
        let mut bytes = vec![0xab; length];
        assert_eq!(bridge::generate(&mut bytes), Code::Invalid);
        assert!(bytes.iter().all(|&byte| byte == 0xab));
    }
}

#[test]
fn initialization_uses_available_mac_or_random_node() {
    let available = mac_address();
    let generator = generator().unwrap();
    if let Some(mac) = available {
        assert_eq!(generator.node, mac);
    } else {
        assert_eq!(generator.node[0] & 1, 1);
    }
}
