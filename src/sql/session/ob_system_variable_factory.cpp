--- a/src/sql/session/ob_system_variable_factory.cpp
+++ b/src/sql/session/ob_system_variable_factory.cpp
@@ -33,4 +33,5 @@ static const char *sys_var_class_type_name[] = {
 };
 
 const int64 ObSystemVariableFactory::kSysVarClassTypeSize[] = {
-    sizeof(ObSysVarClassTypeNull), sizeof(ObSysVarEnableMergeScan), sizeof(ObSysVarMergeScanMinRows), sizeof(ObSysVarMergeScanRowsThreshold)
+    sizeof(ObSysVarClassTypeNull), sizeof(ObSysVarEnableMergeScan), sizeof(ObSysVarMergeScanMinRows), sizeof(ObSysVarMergeScanRowsThreshold),
+    sizeof(ObSysVarEnableFileSql)
 };
@@ -50,4 +51,5 @@ ObSystemVariableFactory::ObSystemVariableFactory()
         new (ptr)ObSysVarEnableMergeScan();
         ptr += sizeof(ObSysVarEnableMergeScan);
         new (ptr)ObSysVarMergeScanMinRows();
         ptr += sizeof(ObSysVarMergeScanMinRows);
-        new (ptr)ObSysVarMergeScanRowsThreshold();
+        new (ptr)ObSysVarMergeScanRowsThreshold();
+        ptr += sizeof(ObSysVarMergeScanRowsThreshold);
+        new (ptr)ObSysVarEnableFileSql();
+        ptr += sizeof(ObSysVarEnableFileSql);
 }
@@ -65,4 +67,6 @@ ObSystemVariableFactory::create_sys_var(ObSQLSessionInfo& session,
             case share::SYS_VAR_MERGE_SCAN_ROWS_THRESHOLD:
                 return create_one_sys_var<share::SYS_VAR_MERGE_SCAN_ROWS_THRESHOLD, ObSysVarMergeScanRowsThreshold>(session, is_virtual, default_value, session_var);
+            case share::SYS_VAR_ENABLE_FILE_SQL:
+                return create_one_sys_var<share::SYS_VAR_ENABLE_FILE_SQL, ObSysVarEnableFileSql>(session, is_virtual, default_value, session_var);
             default:
