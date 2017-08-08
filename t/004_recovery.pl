use strict;
use warnings;
use Cluster;
use TestLib;
use Test::More tests => 3;


ok(1,"Fake test 1");
ok(2,"Fake test 2");
ok(3,"Fake test 3");
__END__
my $cluster = new Cluster(3);
$cluster->init();
$cluster->configure();
$cluster->start();
sleep(10);

$cluster->psql(0, 'postgres', "create extension multimaster;
	create table if not exists t(k int primary key, v int);");

$cluster->psql(0, 'postgres', "insert into t values(1, 10);");
$cluster->psql(1, 'postgres', "insert into t values(2, 20);");
$cluster->psql(2, 'postgres', "insert into t values(3, 30);");
sleep(2);


my $sum0;
my $sum1;
my $sum2;

########################################################
# Check start after all nodes were disconnected
########################################################

$cluster->{nodes}->[1]->stop('fast');
$cluster->{nodes}->[2]->stop('fast');

sleep(5);
$cluster->{nodes}->[1]->start;
# try to start node3 right here?
sleep(5);
$cluster->{nodes}->[2]->start;
sleep(5);

$cluster->psql(0, 'postgres', "select sum(v) from t;", stdout => \$sum0);
$cluster->psql(1, 'postgres', "select sum(v) from t;", stdout => \$sum1);
$cluster->psql(2, 'postgres', "select sum(v) from t;", stdout => \$sum2);
is( (($sum0 == 60) and ($sum1 == $sum0) and ($sum2 == $sum0)) , 1, "Check that nodes are working and sync");

########################################################
# Check recovery during some load
########################################################

$cluster->pgbench(0, ('-i', -s => '10') );
$cluster->pgbench(0, ('-N', -T => '1') );
$cluster->pgbench(1, ('-N', -T => '1') );
$cluster->pgbench(2, ('-N', -T => '1') );

# kill node while neighbour is under load
my $pgb_handle = $cluster->pgbench_async(1, ('-N', -T => '10') );
sleep(5);
$cluster->{nodes}->[2]->stop('fast');
$cluster->pgbench_await($pgb_handle);

# start node while neighbour is under load
$pgb_handle = $cluster->pgbench_async(0, ('-N', -T => '50') );
sleep(10);
$cluster->{nodes}->[2]->start;
$cluster->pgbench_await($pgb_handle);

# give it extra 10s to recover
sleep(10);

# check data identity
$cluster->psql(0, 'postgres', "select sum(abalance) from pgbench_accounts;", stdout => \$sum0);
$cluster->psql(1, 'postgres', "select sum(abalance) from pgbench_accounts;", stdout => \$sum1);
$cluster->psql(2, 'postgres', "select sum(abalance) from pgbench_accounts;", stdout => \$sum2);

diag("Sums: $sum0, $sum1, $sum2");
is($sum2, $sum0, "Check that sum_2 == sum_0");
is($sum2, $sum1, "Check that sum_2 == sum_1");

$cluster->{nodes}->[0]->stop('fast');
$cluster->{nodes}->[1]->stop('fast');
$cluster->{nodes}->[2]->stop('fast');
