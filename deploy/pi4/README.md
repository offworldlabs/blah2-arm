# Opt-in OWL Pi 4B deployment

The default image remains CPU-only. This profile is for the paired OWL OS
Pi 4B changes and a synchronized RSPduo capture at 2 MS/s. The counter scale of
3 and GPU split of 50 are the tested profile's settings, not universal SDR or
Pi settings. Pi 5 has not been tested; maintainers must check its correctness
and performance before merging.

The profile also keeps OpenBLAS and OpenMP at one worker, matching the live
acceptance runs and leaving the four cores for the explicit DSP stage workers.

Build from the same reviewed source snapshot on an ARM64 build host:

```sh
docker build --build-arg OWL_GPU_FIR=OFF -t blah2-pi4-cpu:review .
docker build --build-arg OWL_GPU_FIR=ON -t blah2-pi4-gpu:review .
docker build -f api/Dockerfile -t blah2-pi4-api:review .
```

The GPU image packages Debian's pinned Bookworm-backports Mesa V3D driver and
its dependencies. It needs the host DRM kernel device, not host Mesa files.
Each built image must pass on-device V3D identification, recorded-map and
detection validation, and live deadline/continuity validation. A previous
result with host Mesa does not certify a newly packaged Mesa version.
The SDK remains the existing read-only host library mount from Retina Node.

Load the reviewed images on the node before enabling the override; record
their actual image IDs with `docker image inspect --format '{{.Id}}' IMAGE`.
Resolve the V3D render node on that device, then generate a literal override:

```sh
python3 deploy/pi4/render-override.py \
  --dsp-image "$DSP_IMAGE_ID" --api-image "$API_IMAGE_ID" \
  --render-device "$V3D_RENDER_DEVICE" > pi4-gpu.override.yml
```

The renderer accepts only local `sha256:` image IDs or registry digest
references. It makes no registry publication and uses `pull_policy: never`;
the administrator must preload both exact images. The override changes only
the `blah2` and `blah2_api` services. The managed base retains config-merger,
the tracker sidecar, configuration mounts, and its other services.

For the paired OWL OS build, set `owl_pi4_gpu_stack: True` and provide this
literal file as `owl_pi4_compose_override_src`. OWL installs it on persistent
storage and selects the same file set for boot, GUI, and watchdog operations.
Validate the effective Compose configuration and image identities again after
any managed manifest update. Do not store this selection only in the managed
`.env`, which config-merger replaces.

Keep the current working root and remote-access recovery path until the fresh
image has passed its own checks. A/B partitioning alone does not ensure reset
and reconnection after a failed remote boot.
