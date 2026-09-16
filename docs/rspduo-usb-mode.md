# RSPduo USB transport

The default remains the SDRplay SDK's isochronous transport. Set
`OWL_SDK_USB_MODE=bulk` in the processor environment to opt into bulk transport;
`OWL_SDK_USB_MODE=isoch` selects the default explicitly. Other values, including
an empty string, cause construction to fail before the SDK is opened.

The option only selects USB transport before SDK initialization. It does not
change the ADC/output sample rate, bandwidth, channel count, or DSP profile.
For Docker Compose, set it in the `blah2` service's `environment` mapping.

This opt-in was motivated by Raspberry Pi 4B experiments. The owner does not
have a Pi 5. The integrated patch needs receiver validation and matched Pi 4B
and Pi 5 performance checks; bulk transport is not asserted to be faster on
every platform. Leave it unset to retain the existing behavior.
