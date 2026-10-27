# Server must be local and started with secure_file_priv allowing MYSQLTEST_VARDIR.
--let $file_sql_dir=$MYSQLTEST_VARDIR/file_sql
--mkdir $file_sql_dir
--copy_file $MYSQL_TEST_DIR/test_suite/file_sql/data/people.csv $file_sql_dir/people.csv
--copy_file $MYSQL_TEST_DIR/test_suite/file_sql/data/cities.jsonl $file_sql_dir/cities.jsonl
--copy_file $MYSQL_TEST_DIR/test_suite/file_sql/data/values.parquet $file_sql_dir/values.parquet
--copy_file $MYSQL_TEST_DIR/test_suite/file_sql/data/bad.csv $file_sql_dir/bad.csv
--copy_file $MYSQL_TEST_DIR/test_suite/file_sql/data/nested.jsonl $file_sql_dir/nested.jsonl
