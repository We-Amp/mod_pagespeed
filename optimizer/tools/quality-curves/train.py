#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""
mod_pagespeed — Quality Curve Training Pipeline

Trains per-format LightGBM models that predict encoder quality parameter
for a target SSIMULACRA2 score given 17 image features.

Usage:
    python train.py \
        --features features.csv \
        --sweep sweep_results.csv \
        --output-dir lib/image/generated \
        [--n-trials 100] [--n-splits 5]

Input CSVs are produced by extract_features and quality_sweep tools.
"""

import argparse
import json
from pathlib import Path

import lightgbm as lgb
import numpy as np
import optuna
import pandas as pd
from scipy.interpolate import PchipInterpolator
from sklearn.isotonic import IsotonicRegression
from sklearn.model_selection import GroupKFold

# Feature column names matching ImageFeatures::ToFloatArray() indices 0-16.
FEATURE_COLS = [
    "edge_density",  # 0
    "noise_level",  # 1
    "unique_color_ratio",  # 2
    "photo_metric",  # 3
    "color_entropy",  # 4
    "spatial_freq_low",  # 5
    "spatial_freq_high",  # 6
    "mean_luminance",  # 7
    "luminance_variance",  # 8
    "width",  # 9
    "height",  # 10
    "content_class",  # 11
    "source_format",  # 12
    "has_alpha",  # 13
    "is_grayscale",  # 14
    "target_ssimulacra2",  # 15
    "pixel_count",  # 16
    "source_quality",  # 17
]


def load_data(features_csv: str, sweep_csv: str, fmt: str) -> pd.DataFrame:
    """Load and merge features + sweep data for a single format."""
    features = pd.read_csv(features_csv)
    sweep = pd.read_csv(sweep_csv)

    # Filter sweep to the requested format.
    sweep = sweep[sweep["format"] == fmt].copy()
    if sweep.empty:
        return pd.DataFrame()

    # Merge on path.
    merged = sweep.merge(features, on="path", how="inner")
    return merged


def build_labels(df: pd.DataFrame, target_scores: list[float]) -> pd.DataFrame:
    """
    For each image, fit isotonic regression + PCHIP on (quality → ssimulacra2),
    then interpolate the quality needed for each target score.
    Unreachable targets → NaN (excluded from training).
    """
    rows = []
    for path, group in df.groupby("path"):
        group = group.sort_values("quality")
        qualities = group["quality"].values
        scores = group["ssimulacra2"].values

        # Isotonic regression to enforce monotonicity.
        iso = IsotonicRegression(increasing=True, out_of_bounds="clip")
        scores_mono = iso.fit_transform(qualities, scores)

        # PCHIP interpolation for smooth inverse mapping.
        # We need score → quality, so swap axes.
        # Remove duplicates in scores_mono for interpolation.
        unique_mask = np.diff(scores_mono, prepend=-np.inf) > 0
        if unique_mask.sum() < 2:
            # Not enough unique points for interpolation.
            continue

        interp = PchipInterpolator(
            scores_mono[unique_mask], qualities[unique_mask], extrapolate=False
        )

        score_min = scores_mono[unique_mask].min()
        score_max = scores_mono[unique_mask].max()

        for target in target_scores:
            if target < score_min or target > score_max:
                continue  # Unreachable → skip.
            predicted_q = float(interp(target))
            # Get representative feature row (first occurrence).
            feat_row = group.iloc[0]
            row = {
                "path": path,
                "target_ssimulacra2": target,
                "label_quality": predicted_q,
            }
            # Copy features from CSV (indices 0-14 + source_quality).
            # target_ssimulacra2 and pixel_count are computed, not from CSV.
            for col in FEATURE_COLS[:15]:
                row[col] = feat_row[col]
            if "source_quality" in feat_row.index:
                row["source_quality"] = feat_row["source_quality"]
            else:
                row["source_quality"] = -1  # Legacy CSV without source_quality.
            rows.append(row)

    result = pd.DataFrame(rows)
    if not result.empty:
        result["pixel_count"] = result["width"] * result["height"]
    return result


def train_model(
    train_df: pd.DataFrame,
    n_trials: int,
    n_splits: int,
    fmt: str,
) -> lgb.Booster:
    """Train LightGBM with Optuna hyperparameter search and GroupKFold CV."""
    feature_cols = FEATURE_COLS
    X = train_df[feature_cols].values
    y = train_df["label_quality"].values
    groups = train_df["path"].values

    gkf = GroupKFold(n_splits=n_splits)

    def objective(trial: optuna.Trial) -> float:
        params = {
            "objective": "regression",
            "metric": "mae",
            "verbosity": -1,
            "num_threads": -1,
            "learning_rate": trial.suggest_float("lr", 0.01, 0.3, log=True),
            "num_leaves": trial.suggest_int("num_leaves", 15, 127),
            "min_child_samples": trial.suggest_int("min_child_samples", 5, 50),
            "subsample": trial.suggest_float("subsample", 0.5, 1.0),
            "colsample_bytree": trial.suggest_float("colsample", 0.5, 1.0),
            "reg_alpha": trial.suggest_float("reg_alpha", 1e-8, 10, log=True),
            "reg_lambda": trial.suggest_float("reg_lambda", 1e-8, 10, log=True),
            "max_depth": trial.suggest_int("max_depth", 3, 12),
        }

        maes = []
        for train_idx, val_idx in gkf.split(X, y, groups):
            dtrain = lgb.Dataset(X[train_idx], y[train_idx])
            dval = lgb.Dataset(X[val_idx], y[val_idx], reference=dtrain)
            model = lgb.train(
                params,
                dtrain,
                num_boost_round=500,
                valid_sets=[dval],
                callbacks=[lgb.early_stopping(20, verbose=False)],
            )
            preds = model.predict(X[val_idx])
            mae = np.mean(np.abs(preds - y[val_idx]))
            maes.append(mae)

        return np.mean(maes)

    optuna.logging.set_verbosity(optuna.logging.WARNING)
    study = optuna.create_study(direction="minimize")
    study.optimize(objective, n_trials=n_trials)

    print(f"  [{fmt}] Best CV MAE: {study.best_value:.3f}")
    print(f"  [{fmt}] Best params: {study.best_params}")

    # Retrain on full data with best params.
    best_params = {
        "objective": "regression",
        "metric": "mae",
        "verbosity": -1,
        "learning_rate": study.best_params["lr"],
        "num_leaves": study.best_params["num_leaves"],
        "min_child_samples": study.best_params["min_child_samples"],
        "subsample": study.best_params["subsample"],
        "colsample_bytree": study.best_params["colsample"],
        "reg_alpha": study.best_params["reg_alpha"],
        "reg_lambda": study.best_params["reg_lambda"],
        "max_depth": study.best_params["max_depth"],
    }

    dtrain = lgb.Dataset(X, y)
    model = lgb.train(best_params, dtrain, num_boost_round=500)
    return model


def main():
    parser = argparse.ArgumentParser(description="Train quality prediction models")
    parser.add_argument(
        "--features", required=True, help="features.csv from extract_features"
    )
    parser.add_argument(
        "--sweep", required=True, help="sweep_results.csv from quality_sweep"
    )
    parser.add_argument(
        "--output-dir", required=True, help="Output directory for generated C files"
    )
    parser.add_argument(
        "--n-trials", type=int, default=100, help="Optuna trials per format"
    )
    parser.add_argument("--n-splits", type=int, default=5, help="GroupKFold splits")
    parser.add_argument(
        "--formats", default="jpeg,webp", help="Comma-separated formats to train"
    )
    parser.add_argument(
        "--targets",
        default="40,45,50,55,60,65,70,75,80,85,90",
        help="Comma-separated target SSIMULACRA2 scores",
    )
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    formats = [f.strip() for f in args.formats.split(",")]
    targets = [float(t) for t in args.targets.split(",")]

    for fmt in formats:
        print(f"\n=== Training {fmt} model ===")

        # Load and merge data.
        merged = load_data(args.features, args.sweep, fmt)
        if merged.empty:
            print(f"  No data for format {fmt}, skipping.")
            continue

        print(f"  Loaded {len(merged)} sweep rows for {fmt}")

        # Build interpolated labels.
        train_df = build_labels(merged, targets)
        if train_df.empty:
            print(f"  No valid labels for {fmt}, skipping.")
            continue

        print(
            f"  Built {len(train_df)} training rows ({len(train_df['path'].unique())} images)"
        )

        # Train.
        model = train_model(train_df, args.n_trials, args.n_splits, fmt)

        # Save LightGBM model.
        model_path = output_dir / f"{fmt}_model.txt"
        model.save_model(str(model_path))
        print(f"  Saved model: {model_path}")

        # Save training metadata.
        meta = {
            "format": fmt,
            "n_training_rows": len(train_df),
            "n_images": int(train_df["path"].nunique()),
            "target_scores": targets,
            "feature_names": FEATURE_COLS,
        }
        meta_path = output_dir / f"{fmt}_meta.json"
        with open(meta_path, "w") as f:
            json.dump(meta, f, indent=2)
        print(f"  Saved metadata: {meta_path}")

    print("\nDone. Use compile_model.py to generate C source files.")


if __name__ == "__main__":
    main()
