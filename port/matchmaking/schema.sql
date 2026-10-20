CREATE TABLE IF NOT EXISTS rooms (code TEXT PRIMARY KEY, made INTEGER NOT NULL, version INTEGER NOT NULL, ip TEXT NOT NULL, host_key TEXT NOT NULL, join_key TEXT, host_name TEXT NOT NULL, join_name TEXT, host_addrs TEXT NOT NULL, join_addrs TEXT, queue TEXT, who TEXT, size INTEGER);
CREATE TABLE IF NOT EXISTS used (what TEXT NOT NULL, slot INTEGER NOT NULL, n INTEGER NOT NULL, PRIMARY KEY (what, slot));
-- Rooms for more than two (size 3..10): the players who joined, seat 1 .. size - 1. A database made before these
-- were here also needs, once: ALTER TABLE rooms ADD COLUMN size INTEGER;
CREATE TABLE IF NOT EXISTS seats (code TEXT NOT NULL, seat INTEGER NOT NULL, key TEXT NOT NULL, name TEXT NOT NULL, addrs TEXT NOT NULL, PRIMARY KEY (code, seat));
