# DPS-ESE

##GPU Implementation
###Data source: gpu_stream.py reads your live leader/follower logs and produces latest.json (positions, speed, gap).
###GPU work (Colab): The notebook reads latest.json each cycle, moves data to a CUDA tensor, and runs the cruise‑control math plus a 1000×1000 intruder grid scan on GPU.
###Intruder decision: If the GPU finds an intruder near a follower, it sends a JSON POST back to your local machine.
###Local effect: The follower reads intruder_*.txt and injects EVT_INTRUDER, so the existing C state machine responds.
###So the GPU computes the intruder detection + cruise control math, while the local C code still controls the trucks
