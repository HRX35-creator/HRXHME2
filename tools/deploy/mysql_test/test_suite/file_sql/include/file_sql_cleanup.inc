--remove_file $file_sql_dir/people.csv
--remove_file $file_sql_dir/cities.jsonl
--remove_file $file_sql_dir/values.parquet
--remove_file $file_sql_dir/bad.csv
--remove_file $file_sql_dir/nested.jsonl
--remove_file $file_sql_dir/output.csv
--rmdir $file_sql_dir
SET enable_file_sql=0;
