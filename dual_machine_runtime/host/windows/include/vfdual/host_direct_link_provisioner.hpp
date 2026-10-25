#pragma once

#include "vfdual/host_firewall_provisioner.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vfdual {

enum class HostAdapterTransport { other, ethernet };

/** Address-level evidence retained for deterministic policy and failure logs. */
struct HostNetworkIpv4AddressProfile {
    std::string address;
    std::uint8_t prefix_length{};
    std::uint32_t prefix_origin{};
    std::uint32_t suffix_origin{};
    std::uint32_t dad_state{};
    std::optional<bool> skip_as_source;
};

struct HostNetworkDeviceIdentity {
    bool resolved{};
    std::wstring instance_id;
    std::vector<std::wstring> hardware_ids;
    std::wstring service_name;
    std::wstring driver_inf;
};
[[nodiscard]] bool host_network_identity_is_software_loopback(const HostNetworkDeviceIdentity&) noexcept;
[[nodiscard]] bool host_network_identity_is_resolved(const HostNetworkDeviceIdentity&) noexcept;

/** Read-only adapter facts used by the deterministic direct-link policy. */
struct HostNetworkAdapterProfile {
    std::wstring connection_name;
    std::string adapter_id;
    HostAdapterTransport transport{HostAdapterTransport::other};
    bool hardware_interface{};
    bool operational{};
    bool has_default_gateway{};
    bool has_usable_ipv4{};
    bool has_link_local_ipv4{};
    bool has_ics_private_ipv4{};
    bool has_isolated_private_ipv4{};
    std::uint32_t interface_index{};
    bool dhcp_enabled{};
    std::vector<std::string> ipv4_gateways;
    std::vector<HostNetworkIpv4AddressProfile> ipv4_addresses;
    HostNetworkDeviceIdentity device_identity;
};

/** Unknown metadata may be displayed/probed, but cannot authorize automatic mutation. */
[[nodiscard]] bool host_direct_link_adapter_mutation_is_admissible(const HostNetworkAdapterProfile&) noexcept;

struct HostDirectLinkPlan {
    std::optional<std::size_t> downstream_adapter;
    std::optional<std::size_t> upstream_adapter;
    // This proves only adapter/address readiness. A CAT6 session is not ready
    // until the mobile DHCP/ready heartbeat supplies independent peer evidence.
    bool downstream_already_ready{};
    bool downstream_isolated_pending_preference{};
    bool ambiguous_downstream{};
    std::string reason;
};

/** Pure policy: selects one isolated Ethernet adapter without consulting the Internet uplink. */
[[nodiscard]] HostDirectLinkPlan choose_host_direct_link_plan(
    const std::vector<HostNetworkAdapterProfile>& adapters);

/**
 * Returns true for any 10.57.23.1 address, including tentative/duplicate
 * entries. Restore code must not discard its snapshot while such an entry
 * still exists.
 */
[[nodiscard]] bool host_direct_link_has_any_isolated_address(
    const HostNetworkAdapterProfile& adapter) noexcept;

/** Pure observation only: a restored DHCP lease may legitimately carry a route. */
[[nodiscard]] bool host_direct_link_restored_address_matches(
    const HostNetworkAdapterProfile& adapter, const std::string& snapshot_adapter_id,
    bool originally_dhcp) noexcept;

enum class HostDirectLinkStatus {
    not_required,
    // Adapter/address ready; this status alone never proves a connected peer.
    already_ready,
    provisioned,
    no_wired_adapter,
    no_upstream_adapter,
    requires_elevation,
    conflicting_sharing,
    ambiguous_wired_adapter,
    unavailable,
    failed,
};

struct HostDirectLinkProvisioningResult {
    HostDirectLinkStatus status{HostDirectLinkStatus::not_required};
    std::wstring downstream_name;
    std::wstring upstream_name;
    std::string detail;
    std::string snapshot_path;
    std::string snapshot_sha256;
};

enum class HostDirectLinkMutationStep {
    address,
    dns,
    interface_policy,
    verify,
};

struct HostDirectLinkTransactionResult {
    bool succeeded{};
    HostDirectLinkMutationStep failed_step{HostDirectLinkMutationStep::address};
    std::size_t applied_steps{};
    bool rollback_attempted{};
    bool rollback_succeeded{};
    std::string detail;
};

struct HostDirectLinkTransactionOperations {
    std::function<bool(HostDirectLinkMutationStep, std::string&)> apply;
    std::function<bool(std::string&)> verify;
    std::function<bool(std::string&)> rollback;
};

/**
 * Executes the three direct-link mutations transactionally. This small
 * injectable boundary keeps partial-failure rollback testable without
 * modifying a real Windows interface.
 */
[[nodiscard]] HostDirectLinkTransactionResult execute_host_direct_link_transaction(
    const HostDirectLinkTransactionOperations& operations);

enum class HostDirectLinkAddressBackend { none, transient_ip_helper, persistent_netsh };
enum class HostDirectLinkTransientStatus { created, ownership_conflict, validation_failed, failed };
struct HostDirectLinkAddressPlan {
    HostDirectLinkAddressBackend backend{HostDirectLinkAddressBackend::none};
    bool allow_persistent_fallback{};
    bool preserve_existing_ipv4{};
    std::string reason;
};
/** Address preservation is a constraint; persistent is only a preference. */
[[nodiscard]] HostDirectLinkAddressPlan plan_host_direct_link_address_backend(
    const HostNetworkAdapterProfile& downstream, bool originally_dhcp,
    bool has_static_dns, bool prefer_persistent);
struct HostDirectLinkAddressOperations {
    std::function<bool(HostDirectLinkAddressBackend,std::string&)> record_backend;
    std::function<bool(std::string&)> prepare_private_sharing;
    std::function<HostDirectLinkTransientStatus(std::string&)> create_transient;
    std::function<bool(std::string&)> configure_persistent;
    std::function<bool(std::string&)> configure_dns;
    std::function<bool(std::string&)> configure_interface_policy;
    std::function<bool(std::string&)> verify;
    std::function<bool(HostDirectLinkAddressBackend,std::string&)> rollback;
};
struct HostDirectLinkAddressExecution {
    HostDirectLinkTransactionResult transaction;
    HostDirectLinkAddressBackend applied_backend{HostDirectLinkAddressBackend::none};
};
/** Shared production orchestration; tests inject storage/mutations, never a NIC. */
[[nodiscard]] HostDirectLinkAddressExecution execute_host_direct_link_address_plan(
    const HostDirectLinkAddressPlan& plan,const HostDirectLinkAddressOperations& operations);
/** Read-only guard for pre-existing manual addresses; never reconstructs them. */
[[nodiscard]] bool host_direct_link_preserved_ipv4_matches(
    const std::vector<HostNetworkIpv4AddressProfile>& expected,const HostNetworkAdapterProfile& current) noexcept;

/**
 * Configures only the selected Ethernet link as 10.57.23.1/24 without a
 * gateway or DNS server. Windows ICS/NAT is never enabled.
 *
 * Existing unrelated sharing is never replaced. The operation is outside the
 * capture hot path and is idempotent when the isolated address is already active.
 */
[[nodiscard]] HostDirectLinkProvisioningResult ensure_host_direct_link_ipv4();

/**
 * Runs the same provisioning policy and, when Windows requires it, relaunches
 * this executable as a narrowly scoped elevated worker. The user may see one
 * standard Windows UAC confirmation; no address or adapter entry is required.
 */
[[nodiscard]] HostDirectLinkProvisioningResult ensure_host_direct_link_ipv4_automatically();

/**
 * Reports every 10.57.0.0/16 address currently live on the machine, in the
 * form "10.57.23.1@if13 dad=4 skip=0 | 10.57.49.60@if13", or "none". The CAT6
 * bootstrap watchdog logs transitions of this string so the exact moment and
 * company of the isolated address disappearing is on record.
 */
[[nodiscard]] std::string describe_host_isolated_subnet_state() noexcept;

/** Dumps every IPv4 unicast row on the machine (if/address/dad/skip) for
 * bind-failure forensics. */
[[nodiscard]] std::string describe_host_ipv4_unicast_table() noexcept;

/** When set, the provisioning policy uses the persistent netsh backend even
 * where the transient IP Helper backend would normally be preferred. Used as
 * an A/B discriminator on machines where the transient address dies instantly. */
void set_host_direct_link_prefer_persistent_backend(bool prefer) noexcept;

/** Launches the existing narrow elevated worker used by CAT6 provisioning. */
[[nodiscard]] bool launch_host_direct_link_provisioning_worker(std::string& detail);

enum class HostDirectLinkRestoreStatus {
    not_required,
    restored,
    requires_elevation,
    unsafe_snapshot,
    failed,
};

struct HostDirectLinkRestorationResult {
    HostDirectLinkRestoreStatus status{HostDirectLinkRestoreStatus::not_required};
    std::wstring downstream_name;
    std::string detail;
    std::string snapshot_path;
    std::string snapshot_sha256;
};

/**
 * Restores only the exact physical Ethernet adapter recorded before VisionForge
 * changed it. A fully restored DHCP profile may have its original default route;
 * that case only removes the snapshot. Further mutation rejects routed targets,
 * WiFi and adapter-identity mismatches.
 */
[[nodiscard]] HostDirectLinkRestorationResult restore_host_direct_link_ipv4();
[[nodiscard]] HostDirectLinkRestorationResult restore_host_direct_link_ipv4_automatically();
[[nodiscard]] bool launch_host_direct_link_restore_worker(std::string& detail);
[[nodiscard]] bool launch_host_firewall_provisioning_worker(std::string& detail);

/** Reconciles the complete transport firewall policy, elevating only if required. */
[[nodiscard]] HostFirewallProvisioningResult
ensure_host_firewall_rules_automatically();

/** Entry point used only by the elevated single-EXE provisioning worker. */
[[nodiscard]] int run_host_direct_link_provisioning_worker();
[[nodiscard]] int run_host_direct_link_restore_worker();
[[nodiscard]] int run_host_firewall_provisioning_worker();

inline constexpr wchar_t kHostDirectLinkWorkerArgument[] =
    L"--visionforge-provision-direct-link";
inline constexpr wchar_t kHostDirectLinkRestoreWorkerArgument[] =
    L"--visionforge-restore-direct-link";
inline constexpr wchar_t kHostFirewallWorkerArgument[] =
    L"--visionforge-provision-firewall";

[[nodiscard]] bool host_direct_link_is_ready(HostDirectLinkStatus status) noexcept;
[[nodiscard]] const char* host_direct_link_status_name(HostDirectLinkStatus status) noexcept;
[[nodiscard]] const char* host_direct_link_restore_status_name(
    HostDirectLinkRestoreStatus status) noexcept;
[[nodiscard]] const char* host_direct_link_mutation_step_name(
    HostDirectLinkMutationStep step) noexcept;

}  // namespace vfdual
