# DPS-ESE

###GPU Implementation
- Data source: gpu_stream.py reads your live leader/follower logs and produces latest.json (positions, speed, gap).
- GPU work (Colab): The notebook reads latest.json each cycle, moves data to a CUDA tensor, and runs the cruise‑control math plus a 1000×1000 intruder grid scan on GPU.
- Intruder decision: If the GPU finds an intruder near a follower, it sends a JSON POST back to your local machine.
- Local effect: The follower reads intruder_*.txt and injects EVT_INTRUDER, so the existing C state machine responds.
- So the GPU computes the intruder detection + cruise control math, while the local C code still controls the trucks

- STEPS) - 

- 1 - IN WSL, go to project folder - cd "/mnt/c/Users/RAJDEEP SHAW/Downloads/Distributive and Parallel Systems/project4"
- 2 - Start leader (terminal 1) : stdbuf -oL ./leader | tee leader.log
- 3 - Start follwoer 1 (terminal 2) :  GPU_INTRUDER_FILE=intruder_1.txt stdbuf -oL ./follower 5001 | tee follower1.log
- 4 - Start follower n (terminal n): GPU_INTRUDER_FILE=intruder_1.txt stdbuf -oL ./follower 5001 | tee follower1.log ,
GPU_INTRUDER_FILE=intruder_3.txt stdbuf -oL ./follower 5003 | tee follower3.log
e.t.c
- 5 - Start GPU server in an another terminal - 
python3 gpu_stream.py \
  --leader-log leader.log \
  --follower-log 1:follower1.log \
  --follower-log 2:follower2.log \
  --follower-log 3:follower3.log \
  --serve --port 8000
- 6 - Cloudflare server on another terminal - cloudflared tunnel --url http://localhost:8000
Copy the link that is generated, it is important.
- 7 - In google collab document place in the place of 'LIVE URL'
example: LIVE_URL = "https://<your-tunnel>.trycloudflare.com/latest.json"
- 8 - Check followers and leader terminals to visualise intruder functionalities.
