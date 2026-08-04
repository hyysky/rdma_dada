# rdma2dada

Implemented ingest application and current Linux integration-test entry point.
It loads the JSON data contract, publishes a RAW DADA header, receives fixed
RoCE/UDP frames, strips the 42-byte network header and writes complete raw
records to one PSRDADA ring. It contains composition and lifecycle code only;
transport and ring behavior live under `src/io/`.
