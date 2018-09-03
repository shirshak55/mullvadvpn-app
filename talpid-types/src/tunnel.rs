use std::fmt;

/// Event resulting from a transition to a new tunnel state.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
#[serde(tag = "state", content = "details")]
pub enum TunnelStateTransition {
    /// No connection is established and network is unsecured.
    Disconnected,
    /// Network is secured but tunnel is still connecting.
    Connecting,
    /// Tunnel is connected.
    Connected,
    /// Disconnecting tunnel.
    Disconnecting,
    /// Tunnel is disconnected but secured by blocking all connections.
    Blocked(BlockReason),
}

/// Reason for entering the blocked state.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BlockReason {
    /// Failed to set security policy
    SetSecurityPolicyError,
    /// Failed to start connection to remote server
    StartTunnelError,
}

impl fmt::Display for BlockReason {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        let description = match *self {
            BlockReason::SetSecurityPolicyError => "Failed to set security policy",
            BlockReason::StartTunnelError => "Failed to start connection to remote server",
        };

        write!(formatter, "{}", description)
    }
}