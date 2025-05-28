# my_test.spec — basic isolation test with table and inserts

setup
{
  CREATE TABLE test_table (id int PRIMARY KEY, val text);
}

teardown
{
  DROP TABLE test_table;
}

session s1
setup { BEGIN; }

step s1_insert
{
  INSERT INTO test_table VALUES (1, 'from s1');
}

step s1_commit
{
  COMMIT;
}

session s2
setup { BEGIN; }

step s2_insert
{
  INSERT INTO test_table VALUES (2, 'from s2');
}

step s2_commit
{
  COMMIT;
}

permutation s1_insert s2_insert s1_commit s2_commit
