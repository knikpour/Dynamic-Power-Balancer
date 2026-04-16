# Dynamic Power Balancer (DPB)
A bare-metal Windows background service that dynamically balances CPU and GPU power limits in real-time. Designed for Small Form Factor (SFF) PCs, laptops, and thermally-constrained systems, DPB ensures your system never exceeds its total power supply budget while maximizing gaming and rendering performance.

By reading GPU telemetry via NVIDIA NVML and writing directly to Intel CPU MSRs via WinRing0, DPB aggressively clamps CPU package power the moment your GPU spikes and releases it the moment the GPU idles.

# Disclaimer & Liability
This software writes to physical hardware registers (Ring-0) and overrides factory hardware limits. By downloading, compiling, or running this software, you acknowledge and agree to the following:

Hardware Damage: Incorrect configurations in the config.ini (such as setting extreme undervolts, unlocking IccMax beyond safe limits, or misconfiguring power budgets) can cause system instability, unexpected shutdowns, Blue Screens of Death (BSOD), data loss, and/or permanent physical hardware damage.

Warranty: Using software to bypass factory settings may void your hardware warranties.

No Warranty or Liability: This software is provided "AS IS", without warranty of any kind, express or implied. The author(s) and contributor(s) will not be held liable for any damages, hardware failure, data loss, and/or other consequences arising from the use or misuse of this software.

You are solely responsible for running this software. USE AT YOUR OWN RISK.

# Why This Exists
If you have a 250W power supply, a 150W GPU, and a 125W CPU, running both at 100% load will trip your PSU's Over Current Protection (OCP) and shut down your system. DPB solves this mathematically: Max CPU Power = Total System Budget - Real-Time GPU Power.

Instead of statically crippling your CPU in the BIOS, DPB allows your CPU to boost to its maximum potential during CPU-heavy tasks, seamlessly throttling it only when the GPU demands the wattage.

# Extreme Optimizations
This software is engineered to consume effectively 0.00% CPU, < 1MB RAM, and 0 disk I/O while running.

The balancing loop: contains zero heap allocations and uses strictly integer-based math. Event-Driven OS Hooks drop expensive Ring-0 polling in favor of PowrProf callbacks. Hardware states (Undervolts/Turbo Ratios) are re-applied exactly and only when the NT Kernel broadcasts a Wake-From-Sleep event.

Timer Coalescing: Replaces standard Sleep() with WaitableTimers, allowing the Windows Kernel to group DPB's wake cycles with other background tasks to maintain deep CPU C-States.

Adaptive Polling: Dynamically drops the polling rate from 1,000ms to 5,000ms when the GPU is resting (e.g., < 30W) (these values are all configurable).

Processor Affinity: Supports locking the background loop to a specific CPU core (e.g., E-Cores).

# Configuration6
Because DPB relies on the WinRing0x64.sys driver to write directly to hardware registers, Windows Core Isolation must be disabled. Modern Windows will block this driver by default. Windows Defender may also classifiy those files as malware.

On first launch, DPB generates a config.ini file in its root directory. You can configure:

TOTAL_SYSTEM_BUDGET: The absolute maximum wattage your GPU + CPU can use combined together.

CPU_MAX_WATTS / CPU_MIN_WATTS: Your CPU's dynamic power boundaries.

AFFINITY_MASK: Hex value to pin the process to specific E-Cores.

OFFSET_CPU_CORE_MV: Direct-to-register CPU, Cache, and System Agent undervolting.

ICCMAX_CPU_CORE_A: Unlocking CPU current limits.

# Requirements & Dependencies
OS: Windows 10 / Windows 11 (Requires Administrator privileges / UAC).

GPU: NVIDIA GPU.

CPU: Intel Processor.

Dependencies: WinRing0x64.dll and WinRing0x64.sys must be present in the same directory as the executable.

# Building from Source
Compile using MSVC.
