#!/usr/bin/env python3
"""Render the opt-in OWL Pi 4 profile with immutable, preloaded images.

JSON is valid YAML. Output is deliberately literal so a config-merger rewrite
of the managed .env cannot change the selected images or processing options.
"""
import argparse
import json
import re


def image_reference(value):
    if not re.fullmatch(r"(?:[a-zA-Z0-9][a-zA-Z0-9._:/-]*@)?sha256:[0-9a-f]{64}", value):
        raise argparse.ArgumentTypeError("use a local sha256 image ID or registry name@sha256 digest")
    return value


def render_device(value):
    if not re.fullmatch(r"/dev/dri/renderD[0-9]+", value):
        raise argparse.ArgumentTypeError("use the Pi's verified /dev/dri/renderD* device")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dsp-image", required=True, type=image_reference)
    parser.add_argument("--api-image", required=True, type=image_reference)
    parser.add_argument("--render-device", required=True, type=render_device)
    args = parser.parse_args()
    common = {"pull_policy": "never", "labels": {"io.offworldlabs.pi4-support": "1"}}
    profile = {
        "OPENBLAS_NUM_THREADS": "1",
        "OMP_NUM_THREADS": "1",
        "BLAH2_CAPTURE_BLOCK_QUEUE": "1",
        "BLAH2_VERBOSE": "true",
        "OWL_QUEUE_TWO_CPI": "1",
        "OWL_DIRECT_IQ": "1",
        "OWL_PREPARE_BEFORE_CAPTURE": "1",
        "OWL_SDK_COUNTER_SCALE": "3",
        "OWL_SDK_USB_MODE": "bulk",
        "OWL_CLUTTER_CORR_WORKERS": "2",
        "OWL_CLUTTER_PLAN": "measure",
        "OWL_CLUTTER_DENSE_ONLY": "0",
        "OWL_AMBIG_PLAN": "measure",
        "OWL_AMBIG_WORKERS": "2",
        "OWL_AMBIG_ROWS": "1",
        "OWL_AMBIG_RANGE_THREADS": "2",
        "OWL_GPU_FILTER_PERCENT": "50",
        "VK_ICD_FILENAMES": "/usr/share/vulkan/icd.d/broadcom_icd.json",
    }
    print(json.dumps({"services": {
        "blah2": {**common, "image": args.dsp_image, "environment": profile,
                  "devices": [args.render_device + ":/dev/dri/renderD128"]},
        "blah2_api": {**common, "image": args.api_image},
    }}, indent=2))


if __name__ == "__main__":
    main()
