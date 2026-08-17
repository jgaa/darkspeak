-- DarkSpeak fresh schema, version 1. This is intentionally independent of
-- database/master.db, which is retained as historical reference material.
PRAGMA foreign_keys = ON;

CREATE TABLE schema_version (version INTEGER NOT NULL CHECK (version = 1));
INSERT INTO schema_version(version) VALUES (1);

CREATE TABLE identity (
    id INTEGER PRIMARY KEY,
    uuid BLOB NOT NULL UNIQUE,
    name TEXT NOT NULL UNIQUE,
    cert BLOB NOT NULL,
    address TEXT,
    created TEXT NOT NULL
    ,bio TEXT
    ,bio_hash BLOB
);
CREATE TABLE contact (
    id INTEGER PRIMARY KEY,
    identity_id INTEGER NOT NULL REFERENCES identity(id),
    uuid BLOB NOT NULL UNIQUE,
    name TEXT,
    cert BLOB NOT NULL,
    address TEXT NOT NULL,
    created TEXT NOT NULL
);
CREATE TABLE conversation (
    id INTEGER PRIMARY KEY,
    identity_id INTEGER NOT NULL REFERENCES identity(id),
    name TEXT NOT NULL,
    created TEXT NOT NULL,
    updated TEXT NOT NULL
);
CREATE TABLE message (
    id INTEGER PRIMARY KEY,
    conversation_id INTEGER NOT NULL REFERENCES conversation(id) ON DELETE CASCADE,
    message_id BLOB NOT NULL UNIQUE,
    direction INTEGER NOT NULL,
    state INTEGER NOT NULL,
    content TEXT NOT NULL,
    composed_time INTEGER NOT NULL,
    signature BLOB NOT NULL
);
CREATE TABLE file (
    id INTEGER PRIMARY KEY,
    file_id BLOB NOT NULL UNIQUE,
    identity_id INTEGER NOT NULL REFERENCES identity(id),
    contact_id INTEGER NOT NULL REFERENCES contact(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    path TEXT,
    size INTEGER NOT NULL CHECK (size >= 0),
    hash BLOB,
    bytes_transferred INTEGER NOT NULL DEFAULT 0 CHECK (bytes_transferred >= 0)
);
