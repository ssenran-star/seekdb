--- a/src/sql/session/ob_system_variable_factory.h
+++ b/src/sql/session/ob_system_variable_factory.h
@@ -158,4 +158,8 @@ class ObSysVarMergeScanRowsThreshold : public ObIntSysVar
     }
 };
 
+class ObSysVarEnableFileSql : public ObBoolSysVar
+{
+public: ObSysVarEnableFileSql() : ObBoolSysVar("enable_file_sql", false, GLOBAL_VAR | SESSION_VAR, false, true, NULL) {}
+};
