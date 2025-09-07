# Known Issues

## Parents Column Crash with Large Result Sets

### Issue
When using the `parents` column with ORDER BY or aggregate functions on large result sets (>200 rows), PostgreSQL crashes with a segmentation fault.

### Symptoms
- Works fine for small queries (<200 rows)
- Crashes when sorting/aggregating larger result sets
- Example query that crashes:
  ```sql
  SELECT fid, jsonb_array_length(parents) as cnt 
  FROM gd_test_parents 
  WHERE type = 0 
  ORDER BY jsonb_array_length(parents) DESC 
  LIMIT 5;
  ```

### Workaround
Use a subquery to limit the number of rows before sorting:
```sql
SELECT fid, jsonb_array_length(parents) as cnt 
FROM (
  SELECT fid, parents 
  FROM gd_test_parents 
  WHERE type = 0 
  LIMIT 200
) sub 
ORDER BY jsonb_array_length(parents) DESC 
LIMIT 5;
```

### Analysis
The crash appears to be related to memory management when PostgreSQL needs to keep many parent JSONB values in memory for sorting. Possible causes:
1. Memory leak in parent iteration
2. libgeodesk issue with calling `parents()` on many features
3. PostgreSQL memory context not being properly managed

### TODO
- Investigate memory context switching for per-tuple allocation
- Add debugging to track memory usage during parent extraction
- Consider caching parent data to avoid repeated calls
- Test with valgrind to identify memory issues