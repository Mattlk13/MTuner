@echo off
rem Translation workflow. Override QTBIN to point at a different Qt if needed.
if not defined QTBIN set QTBIN=C:\Qt\6.10.2\msvc2022_64\bin

rem -locations none keeps the .ts free of churn-prone line numbers; the .ts are
rem kept in this lupdate's canonical context layout, so runs round-trip cleanly
rem (no translations are dropped). Re-baseline before changing Qt versions.
"%QTBIN%\lupdate.exe" -no-obsolete -locations none mtunerlang.pro

rem Open Linguist to fill any newly-added (unfinished) strings, then release.
"%QTBIN%\linguist.exe" mtuner_en.ts mtuner_rs.ts mtuner_it.ts mtuner_de.ts mtuner_zh.ts mtuner_ja.ts

"%QTBIN%\lrelease.exe" mtunerlang.pro
