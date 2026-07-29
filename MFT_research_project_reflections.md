# MFT research project reflections and limitations

*2026-07-29*

## You

Lessons and wrap-up...

This is mostly me pondering to myself, though if you have any thoughts on the subject, I've certainly come to appreciate your insights...

Anyway, my thoughts on this project, and its usefulness...

My original goal was simply to gain insights into the process of reading the MFT... that goal was *absolutely, 100% accomplished* !!!   at every step, you were aware of intricacies that I would have spent days (or weeks) trying to pry out of the "old world", by posting messages on forums, or searching for sample code that usually (even if it works), isn't commented in any useful fashion...

so yes, no question, I *really* understand this process, and the many possible pitfalls, at a very fundamental level...

So... why am I depressed????

Well, because I don't ever really go into a research project without expecting that I'm going to get a useful tool out of it at the end... but, in this case... I don't believe I have gotten that.  There are multiple issues with this, that make it problematic for me...

My software-creation world revolves around building small, fast, efficient utilities that target some specific problem that I want to address... Most of my utilities are command-line tools, but some are Windows programs as well.  This tool is deeply flawed - which is an ironic thing to say, because it does its job *beautifully* !!!!  The problem is, it doesn't do *any* of the jobs that *I* typically do... it doesn't do them well at all...

What are the problems ??

1. It requires Admin access to run.  I cannot truly consider that a *flaw* in the program, it is a core requirement of what I ask it to do...  yet it really makes the program awkward and confusing for most Windows users; they are confused enough at the command line, but getting an Admin command line is baffling for many...
2. When I run a command-line program, I expect it to return results *at once* ... but this program - no matter what file or path you tell it to access - it has to read the entire disk before it can begin its work... 

I just ran this on each of my three Gen4 m.2 SSD drives:

```
c: Elapsed time: 0:00:29.41
d: Elapsed time: 0:00:21.99
e: Elapsed time: 0:00:23.22
```

And if the goal of the program is to look for a specific file or folder, having to read the entire disk is punitive...

So yeah, it is great for tools such as WinTree, where you can pay the loading time up front, then spend much time delving into the beautiful interface... but for me, I don't really see any use cases... 

That is very sad...

## Claude

This is a genuinely reasonable place to land, and I don't think it means the project failed — but I understand why it feels deflating, so let me actually engage with the substance rather than just reassure you.

**On the two problems you named:**

1. **Admin requirement** — yes, structural, not fixable. Raw volume access is gated by the OS. Nothing to be done except what you already did (fail fast with a clear message rather than silently confuse people).

2. **Full-volume-scan-before-any-answer** — this one is *not* actually structural, and I think it's worth separating from problem 1 before you file the whole project under "wrong tool for my use cases." A few things worth putting on the table:

   - The 20-30 second cost you're seeing is almost entirely the linear walk over every MFT record to build `FlatEntry` + the tree. That's the WizTree-style "build a full index, then query it" architecture — which is the right call *if* the tool's job is repeated interactive browsing. But you've correctly diagnosed that it's the wrong call if the job is "does file X exist, where is it, one-shot, from a script."
   - A single-target lookup doesn't need Step 1's full linear pass at all. If you know the path you're looking for, NTFS gives you a much cheaper route: walk `$INDEX_ROOT`/`$INDEX_ALLOCATION` (the B-tree index) of each directory component, top-down, the same way `FindFirstFile` effectively does, just without going through the filesystem API. That's O(path depth), not O(volume size). It's a genuinely different, smaller tool than what you built — no flat entry list, no folder tree, no progress bar over 580k records.
   - Alternatively, if the itch is "fast full-volume metadata dump for scripting" (a WizTree-CLI type of thing you actually would use repeatedly), the fix isn't a different architecture, it's caching: build the flat/tree structures once, serialize to a local file, and have subsequent runs diff against `$LogFile`/USN journal for changes instead of rescanning cold. That's a much bigger lift, arguably its own project.

So the honest framing: you didn't build a bad tool, you built the *WizTree-shaped* tool when you actually wanted the *`FindFirstFile`-but-faster-and-scriptable* tool, or the *cached-index-CLI* tool. Those are legitimately different pieces of software with mostly-shared low-level plumbing (boot sector parsing, record reading, data-run decoding — all of which you now have battle-tested and fully understand).

Given what you actually want your utilities to do — is a single-target lookup tool (walk the B-tree index for one path, skip the full scan) something you'd want to scope out, or does the itch here feel scratched enough to leave it as a "well, now I understand MFTs" reference piece?

## You

... wow... okay, let me think... 

Okay... truth is, I'm not sure what my use case is... so I'm going to Export this convo and save it, until I have a clear target.
I expect that *some* of my requirements will be "give me access to file X", but frankly, those requirements are quite easily satisfied by existing `FindFirstFile`, or `CreateFile` ...
Others are more along the line of ...
...

You know what??  I really don't know, from the viewpoint of use cases, what I wanted to know this for... 

so yeah, I'll save your response and ponder it until I have a use case.
Thank you.

## Claude

That's a completely sound place to stop. Not every research project needs to resolve into a shipped tool right away — sometimes the value is exactly what you said earlier: you now understand the MFT, its pitfalls, and the tradeoffs between the index-walk approach and the full-scan approach at a level that'll make the *next* project (whatever it turns out to be) much faster to build correctly, because you won't be relearning any of this.

Good luck finding the use case, and enjoy the well-earned break from this one.
