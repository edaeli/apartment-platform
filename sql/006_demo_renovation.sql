-- Старые характеристики/цены остаются без изменений. Обновление демо — отдельная команда.
ALTER TABLE properties ADD COLUMN renovation TEXT NOT NULL DEFAULT 'unspecified'
    CHECK (renovation IN ('unspecified', 'needs_repair', 'cosmetic', 'good', 'designer'));
-- Отметка версии генератора: повторное обновление не затрагивает уже обработанный объект.
ALTER TABLE properties ADD COLUMN demo_revision INTEGER NOT NULL DEFAULT 0
    CHECK (demo_revision IN (0, 2));
PRAGMA user_version = 6;
