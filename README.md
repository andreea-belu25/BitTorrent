# BitTorrent-like File Sharing Implementation

In the implementation logic, I do not distinguish between seeds and peers.

## 1. Data Structures Used

### TrackerData
- Maintains swarms, file information, and the number of files
- **Swarm**: Stores the number of clients and the clients that possess the files
- **FileInfoTracker**: Contains file information (name, segment hashes stored in Segment, number of segments)

### PeerData
- Maintains the number of files owned by a client
- Stores the files owned by the client
- Tracks the number of files the client wants
- Records what the client wants from these files
- Stores the client's rank
- **FileInfoPeer**: Contains information about owned files (file name, number of segments owned, and hashes of owned segments)

### ClientStress
- Indicates how heavily solicited a client is (necessary for efficiency)

## 2. Implementation Logic

### Tracker

**Receiving files from clients** (`receiveFilesFromClients()`):
- For each client, receives the number of files owned by that client
- For each file:
  - Checks if the file already exists (`checkIfFileExists()`)
    - If it doesn't exist: initializes file information (`initFile()`) and associated swarm (`initSwarm()`), then increments the number of managed files
    - If it already exists: continues receiving information from clients but does nothing with it
  - Checks if the current client already belongs to the swarm (`addClientIfNotExistsSwarm()`), and adds it if not
- After receiving all files from clients, sends a notification to each client (`startClients()`), signaling that file uploading and downloading can begin

**Handling messages** (`handleMessages()`):
- Acts as an intermediary between clients, managing the flow of messages and requests
- For each received file, checks the channel (tag) to determine the next action:
  - **Tag 3**: Request to receive segments
    - `reqFileSwarm()` provides the file-specific swarm and adds the client to the swarm if not already present
  - **Tag 4**: Sends information about requested file segments via `reqFileInfo()`
  - **Tag 5**: File has been completely downloaded; no action taken (no distinction between peers and seeds)
  - **Tag 6**: All files requested by a client have been downloaded; increments the count of clients who have finished downloading
- When all clients have finished downloading:
  - Tracker stops and sends each client a message to indicate they can shut down (`finishedDownloading()`)

### Client

**Initialization**:
- Reads the file specific to its rank and saves the data using `readFile()`
- Sends files to tracker via `sendFilesToTracker()`:
  - For each owned file, sends the file name, number of segments, and the hash for each segment
  - Waits for a message from the tracker to begin searching and downloading desired files

**Upload thread** (`upload_thread_func()`):
- Checks if a shutdown message has been received from the tracker
- If the client possesses the requested segment, sends 1
- Otherwise, notifies that it doesn't possess the requested segment by sending 0

**Download thread** (`download_thread_func()`):
- For each desired file:
  - Requests hashes from the tracker
  - Updates the number of owned files
  - Initializes the file in the list of owned files
  - Updates the swarm after downloading 10 segments (`getSwarmForFile()`)
  - Uses a list where clients are sorted in ascending order by their solicitation level or rank (`sortClients()`)
  - Searches to see if the respective client has the desired hash; if so, downloads from that client
  - Stops searching when a client with the desired hash is found
- When a client finishes downloading:
  - Saves the respective file in a format matching the specification (`saveFile()`)

## 3. Communication Details

- Requests (`MPI_Send`) and receptions (`MPI_Recv`) for different information are handled through different channels (marked with different tags in the code)
