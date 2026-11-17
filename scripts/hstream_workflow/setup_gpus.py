"""Resolve saved GPU-family choices against Slurm's current node inventory."""

import re

from .schema import require

_TYPED_GPU = re.compile(r"gpu:([A-Za-z0-9_.+\-]+):([0-9]+)")


def needs_inventory(profiles):
    return any(
        _TYPED_GPU.fullmatch(profile["gres"])
        or re.fullmatch(r"gpu:[0-9]+", profile["gres"])
        for profile in profiles.values()
    )


def resolve_profiles(profiles, inventory):
    machines = {}
    for line in inventory.splitlines():
        fields = [field.strip() for field in line.split("|")]
        require(
            len(fields) == 3,
            "Slurm returned an unreadable computer list. Find my computers again.",
        )
        node, partition, resources = fields
        # Socket annotations may themselves contain commas. They do not alter
        # the node's declared GPU type or count.
        resources = re.sub(r"\([^)]*\)", "", resources)
        gpus = {}
        for item in resources.split(","):
            match = _TYPED_GPU.fullmatch(item)
            if match:
                gpu, count = match.groups()
                gpus[gpu] = gpus.get(gpu, 0) + int(count)
        key = (node, partition.removesuffix("*"))
        require(
            key not in machines or machines[key] == gpus,
            "Slurm reported conflicting GPUs for "
            + node
            + ". Find my computers again.",
        )
        machines[key] = gpus

    resolved, messages = {}, []
    for key, profile in profiles.items():
        request = profile["gres"]
        match = _TYPED_GPU.fullmatch(request)
        choices = resolved[key] = {}
        for node in profile["nodes"]:
            choices[node] = request
            untyped = re.fullmatch(r"gpu:([0-9]+)", request)
            # Compound/custom GPU requests retain their exact meaning.
            if not match and not untyped:
                continue
            wanted = match.group(1) if match else None
            count_text = match.group(2) if match else untyped.group(1)
            count = int(count_text)
            available = {}
            partitions = profile["partition"].split(",")
            found = False
            for partition in partitions:
                if (node, partition) in machines:
                    found = True
                    for gpu, capacity in machines[(node, partition)].items():
                        available[gpu] = max(available.get(gpu, 0), capacity)
            prefix = profile["label"] + " · " + node + ": "
            if untyped:
                # A single reported type makes the generic and typed choices
                # equivalent. Leave mixed/unknown GPU inventories unchanged.
                if len(available) == 1 and next(iter(available.values())) >= count > 0:
                    choices[node] = "gpu:" + next(iter(available)) + ":" + count_text
                continue
            require(
                found,
                prefix
                + "this computer is not in the selected queue. Find my computers again and choose an available queue.",
            )
            if wanted in available:
                candidates = [wanted] if available[wanted] >= count else []
            elif re.fullmatch(r"[A-Za-z][A-Za-z_-]*", wanted):
                # A saved family such as rtx can narrow to the actual model.
                # Numeric model prefixes are not families: A10 must never
                # silently become A100 when an A10 is no longer present.
                candidates = sorted(
                    gpu
                    for gpu, capacity in available.items()
                    if gpu.startswith(wanted) and capacity >= count
                )
            else:
                candidates = []
            listed = ", ".join(sorted(available)) or "no typed GPUs"
            require(
                len(candidates) == 1 and count > 0,
                prefix
                + "the saved GPU choice "
                + request
                + (
                    " matches several GPU types"
                    if len(candidates) > 1
                    else " is unavailable"
                )
                + ". This computer reports "
                + listed
                + ". Open Advanced profile settings and choose an exact GPU type, or remove this computer from the choice.",
            )
            if candidates[0] != wanted:
                choices[node] = "gpu:" + candidates[0] + ":" + count_text
            if choices[node] != request:
                messages.append(
                    prefix
                    + "updated the saved GPU choice from "
                    + request
                    + " to "
                    + choices[node]
                    + "."
                )
    return resolved, messages
