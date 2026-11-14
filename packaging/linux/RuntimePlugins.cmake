# SPDX-License-Identifier: MPL-2.0
# The product uses QSQLITE. Vendor SQL drivers require separately installed clients
# (and separate distribution review); do not ship unusable optional plugins.
# Serial/NMEA hardware positioning is also unused and depends on optional QtSerialPort.
set(ketplus_linux_excluded_runtime_plugins
    qsqlibase qsqlmimer qsqlmysql qsqloci qsqlodbc qsqlpsql qtposition_nmea)
