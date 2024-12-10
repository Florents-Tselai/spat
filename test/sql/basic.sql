SET client_min_messages = ERROR;

SELECT spat_db_name(); -- spat-default
-- switch to a new db
SET spat.db = 'test-spat';
SELECT spat_db_name(); -- test-spat
SELECT spat_db_created_at() NOTNULL;

-- switch back to the default to preserve my sanity when copy-pasting

SET spat.db = 'spat-default';

SELECT spat_db_set_int('key1', 1);
SELECT spat_db_get_int('key1');
SELECT spat_db_get_int('keaaaaay1');
SELECT spat_db_set_int('keaaaaay1', 1000);
SELECT spat_db_get_int('keaaaaay1');

select spat_db_type('key1');
select spat_db_type('kgdsfgdsfhdfey1');
