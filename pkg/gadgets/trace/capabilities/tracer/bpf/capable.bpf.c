// SPDX-License-Identifier: GPL-2.0
//
// Unique filtering based on
// https://github.com/libbpf/libbpf-rs/tree/master/examples/capable
//
// Copyright 2022 Sony Group Corporation

#include <vmlinux/vmlinux.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "capable.h"

#define MAX_ENTRIES	10240

const volatile pid_t my_pid = -1;
const volatile enum uniqueness unique_type = UNQ_OFF;
const volatile pid_t targ_pid = -1;
const volatile bool filter_by_mnt_ns = false;

struct unique_key {
	int cap;
	u32 tgid;
	u64 cgroupid;
};

struct {
	__uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, sizeof(__u32));
} events SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, struct key_t);
	__type(value, struct cap_event);
	__uint(max_entries, MAX_ENTRIES);
} info SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 10240);
	__type(key, struct unique_key);
	__type(value, u64);
} seen SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__uint(key_size, sizeof(u64));
	__uint(value_size, sizeof(u32));
} mount_ns_filter SEC(".maps");

SEC("kprobe/cap_capable")
int BPF_KPROBE(ig_trace_cap, const struct cred *cred, struct user_namespace *targ_ns, int cap, int cap_opt)
{
	__u32 pid;
	u64 mntns_id;
	__u64 pid_tgid;
	struct key_t i_key;
	struct task_struct *task;

	task = (struct task_struct*) bpf_get_current_task();
	mntns_id = (u64) BPF_CORE_READ(task, nsproxy, mnt_ns, ns.inum);

	if (filter_by_mnt_ns && !bpf_map_lookup_elem(&mount_ns_filter, &mntns_id))
		return 0;

	pid_tgid = bpf_get_current_pid_tgid();
	pid = pid_tgid >> 32;

	if (pid == my_pid)
		return 0;

	if (targ_pid != -1 && targ_pid != pid)
		return 0;

	struct cap_event event = {};
	event.pid = pid;
	event.tgid = pid_tgid;
	event.cap = cap;
	event.uid = bpf_get_current_uid_gid();
	event.mntnsid = mntns_id;
	event.cap_opt = cap_opt;
	bpf_get_current_comm(&event.task, sizeof(event.task));

	if (unique_type) {
		struct unique_key key = {.cap = cap};
		if (unique_type == UNQ_CGROUP) {
			key.cgroupid = bpf_get_current_cgroup_id();
		} else {
			key.tgid = pid_tgid;
		}

		if (bpf_map_lookup_elem(&seen, &key) != NULL) {
			return 0;
		}
		u64 zero = 0;
		bpf_map_update_elem(&seen, &key, &zero, 0);
	}

	bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &event, sizeof(event));

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
