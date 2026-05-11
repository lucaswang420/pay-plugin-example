-- Pay Plugin Database Schema
-- Initial table creation with auto-update triggers for updated_at

-- 1. Create the trigger function first
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$ language 'plpgsql';

-- 2. Create tables
CREATE TABLE IF NOT EXISTS pay_order (
    id BIGSERIAL PRIMARY KEY,
    order_no VARCHAR(64) UNIQUE NOT NULL,
    user_id BIGINT NOT NULL,
    amount VARCHAR(32) NOT NULL,
    currency VARCHAR(8) NOT NULL DEFAULT 'CNY',
    status VARCHAR(32) NOT NULL DEFAULT 'pending',
    channel VARCHAR(32) NOT NULL DEFAULT 'alipay',
    title VARCHAR(512),
    expire_at TIMESTAMP,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS pay_payment (
    id BIGSERIAL PRIMARY KEY,
    payment_no VARCHAR(64) UNIQUE NOT NULL,
    order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),
    status VARCHAR(32) NOT NULL DEFAULT 'pending',
    amount VARCHAR(32) NOT NULL,
    request_payload TEXT,
    response_payload TEXT,
    channel_trade_no VARCHAR(64),
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS pay_refund (
    id BIGSERIAL PRIMARY KEY,
    refund_no VARCHAR(64) UNIQUE NOT NULL,
    order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),
    payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),
    status VARCHAR(32) NOT NULL DEFAULT 'pending',
    amount VARCHAR(32) NOT NULL,
    channel_refund_no VARCHAR(64),
    request_payload TEXT,
    response_payload TEXT,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS pay_callback (
    id BIGSERIAL PRIMARY KEY,
    payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),
    raw_body TEXT NOT NULL,
    signature VARCHAR(512),
    serial_no VARCHAR(64),
    verified BOOLEAN NOT NULL DEFAULT FALSE,
    processed BOOLEAN NOT NULL DEFAULT FALSE,
    received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
    -- Note: callbacks are generally insert-only, so no updated_at needed here
);

CREATE TABLE IF NOT EXISTS pay_idempotency (
    idempotency_key VARCHAR(128) PRIMARY KEY,
    request_hash VARCHAR(64) NOT NULL,
    response_snapshot TEXT,
    expire_at TIMESTAMP,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS pay_ledger (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL,
    order_no VARCHAR(64) NOT NULL,
    payment_no VARCHAR(64),
    entry_type VARCHAR(32) NOT NULL,
    amount VARCHAR(32) NOT NULL,
    balance VARCHAR(32),
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
    -- Note: ledger entries are immutable (append-only), so no updated_at needed here
);

-- 3. Attach triggers to tables that have updated_at
DROP TRIGGER IF EXISTS update_pay_order_modtime ON pay_order;
CREATE TRIGGER update_pay_order_modtime
    BEFORE UPDATE ON pay_order
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

DROP TRIGGER IF EXISTS update_pay_payment_modtime ON pay_payment;
CREATE TRIGGER update_pay_payment_modtime
    BEFORE UPDATE ON pay_payment
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

DROP TRIGGER IF EXISTS update_pay_refund_modtime ON pay_refund;
CREATE TRIGGER update_pay_refund_modtime
    BEFORE UPDATE ON pay_refund
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

DROP TRIGGER IF EXISTS update_pay_idempotency_modtime ON pay_idempotency;
CREATE TRIGGER update_pay_idempotency_modtime
    BEFORE UPDATE ON pay_idempotency
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();
