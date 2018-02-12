#!/bin/bash -l

set -eox pipefail

./ccp_src/aws/setup_ssh_to_cluster.sh

CLUSTER_NAME=$(cat ./cluster_env_files/terraform/name)

if [ "$TEST_OS" = centos6 ]; then
    CGROUP_BASEDIR=/cgroup
else
    CGROUP_BASEDIR=/sys/fs/cgroup
fi

if [ "$TEST_OS" = centos7 -o "$TEST_OS" = sles12 ]; then
    CGROUP_AUTO_MOUNTED=1
fi

mount_cgroups() {
    local gpdb_host_alias=$1
    local basedir=$CGROUP_BASEDIR
    local options=rw,nosuid,nodev,noexec,relatime
    local groups="cpuset blkio cpuacct cpu memory"

    if [ "$CGROUP_AUTO_MOUNTED" ]; then
        # nothing to do as cgroup is already automatically mounted
        return
    fi

    ssh -t $gpdb_host_alias sudo bash -ex <<EOF
        mkdir -p $basedir
        mount -t tmpfs tmpfs $basedir
        for group in $groups; do
                mkdir -p $basedir/\$group
                mount -t cgroup -o $options,\$group cgroup $basedir/\$group
        done
EOF
}

make_cgroups_dir() {
    local gpdb_host_alias=$1
    local basedir=$CGROUP_BASEDIR

    ssh -t $gpdb_host_alias sudo bash -ex <<EOF
        for comp in cpu cpuacct memory; do
            chmod -R 777 $basedir/\$comp
            mkdir -p $basedir/\$comp/gpdb
            chown -R gpadmin:gpadmin $basedir/\$comp/gpdb
            chmod -R 777 $basedir/\$comp/gpdb
        done
EOF
}

mount_cgroups ccp-${CLUSTER_NAME}-0
mount_cgroups ccp-${CLUSTER_NAME}-1
make_cgroups_dir ccp-${CLUSTER_NAME}-0
make_cgroups_dir ccp-${CLUSTER_NAME}-1
