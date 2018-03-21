#!/bin/bash

set -eox pipefail

CLUSTER_NAME=$(cat ./cluster_env_files/terraform/name)

if [ "$TEST_OS" = centos6 ]; then
    CGROUP_BASEDIR=/cgroup
else
    CGROUP_BASEDIR=/sys/fs/cgroup
fi

if [ "$TEST_OS" = centos7 -o "$TEST_OS" = sles12 ]; then
    CGROUP_AUTO_MOUNTED=1
fi


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

make_cgroups_dir ccp-${CLUSTER_NAME}-0
make_cgroups_dir ccp-${CLUSTER_NAME}-1

pushd gpdb_src
./configure --prefix=/usr/local/greenplum-db-devel \
            --without-zlib --without-rt --without-libcurl \
            --without-libedit-preferred --without-docdir --without-readline \
            --disable-gpcloud --disable-gpfdist --disable-orca \
            --disable-pxf
popd
pushd gpdb_src/src/test/isolation2
make
scp -r pg_isolation2_regress mdw:/usr/local/greenplum-db-devel/lib/postgresql/pgxs/src/test/regress/
scp -r ../regress/gpstringsubs.pl mdw:/usr/local/greenplum-db-devel/lib/postgresql/pgxs/src/test/regress/
scp -r ../regress/gpdiff.pl mdw:/usr/local/greenplum-db-devel/lib/postgresql/pgxs/src/test/regress/
scp -r ../regress/atmsort.pm mdw:/usr/local/greenplum-db-devel/lib/postgresql/pgxs/src/test/regress/
scp -r ../regress/explain.pm mdw:/usr/local/greenplum-db-devel/lib/postgresql/pgxs/src/test/regress/
popd


scp -r plcontainer_gpdb_build mdw:/tmp/
scp -r plcontainer_src mdw:~/
ssh mdw "bash -c \" \
set -eox pipefail; \
export MASTER_DATA_DIRECTORY=/data/gpdata/master/gpseg-1; \
source /usr/local/greenplum-db-devel/greenplum_path.sh; \
gppkg -i /tmp/plcontainer_gpdb_build/plcontainer*.gppkg; \
\""

scp -r plcontainer_pyclient_docker_image/plcontainer-*.tar.gz mdw:/usr/local/greenplum-db-devel/share/postgresql/plcontainer/plcontainer-python-images.tar.gz
scp -r plcontainer_rclient_docker_image/plcontainer-*.tar.gz mdw:/usr/local/greenplum-db-devel/share/postgresql/plcontainer/plcontainer-r-images.tar.gz

ssh mdw "bash -c \" \
set -eox pipefail; \
export MASTER_DATA_DIRECTORY=/data/gpdata/master/gpseg-1; \
source /usr/local/greenplum-db-devel/greenplum_path.sh; \

plcontainer image-add -f /usr/local/greenplum-db-devel/share/postgresql/plcontainer/plcontainer-python-images.tar.gz; \
plcontainer image-add -f /usr/local/greenplum-db-devel/share/postgresql/plcontainer/plcontainer-r-images.tar.gz; \

export MASTER_DATA_DIRECTORY=/data/gpdata/master/gpseg-1; \
gpconfig -c gp_resource_manager -v "group"; \
gpstop -arf; \
psql -d postgres -f /usr/local/greenplum-db-devel/share/postgresql/plcontainer/plcontainer_install.sql; \
psql -d postgres -c \"create resource group plgroup with(concurrency=0,cpu_rate_limit=10,memory_limit=30,memory_auditor='cgroup');\"; \
psql -d postgres -c \"alter resource group admin_group set memory_limit 35;\"; \

groupid=`psql -d postgres -t -q -c "select groupid from gp_toolkit.gp_resgroup_config where groupname='plgroup';"`; \
groupid=`echo $groupid | awk '{$1=$1};1'`; \
plcontainer runtime-add -r plc_python_shared -i pivotaldata/plcontainer_python_shared:devel -l python -s use_container_logging=yes -s resource_group_id=${groupid}; \
plcontainer runtime-add -r plc_r_shared -i pivotaldata/plcontainer_r_shared:devel -l r -s use_container_logging=yes -s resource_group_id=${groupid}; \

pushd plcontainer_src/tests/isolation2; \
make resgroup; \
popd; \
pushd plcontainer_src/tests; \
timeout -s 9 60m make resgroup; \
popd; \
\""
